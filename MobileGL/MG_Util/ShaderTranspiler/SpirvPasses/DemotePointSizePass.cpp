// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/DemotePointSizePass.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "DemotePointSizePass.h"

#include "spirv.hpp"
#include "source/opt/constants.h"
#include "source/opt/def_use_manager.h"
#include "source/opt/instruction.h"
#include "source/opt/ir_context.h"
#include "source/opt/module.h"
#include "source/opt/type_manager.h"
#include "source/opt/types.h"
#include "source/util/make_unique.h"
#include "source/util/string_utils.h"

#include <format>
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

                Instruction* EntryPoint(IRContext* ctx) {
                    for (Instruction& ep : ctx->module()->entry_points()) {
                        return &ep;
                    }
                    return nullptr;
                }

                // OpTypePointer <storage-class> <pointee>
                uint32_t VariablePointeeType(IRContext* ctx, Instruction* var) {
                    Instruction* ptrType = ctx->get_def_use_mgr()->GetDef(var->type_id());
                    if (ptrType == nullptr || ptrType->opcode() != spv::Op::OpTypePointer) return 0;
                    return ptrType->GetSingleWordInOperand(1);
                }

                bool IsFloat32Type(IRContext* ctx, uint32_t typeId) {
                    Instruction* t = ctx->get_def_use_mgr()->GetDef(typeId);
                    return t != nullptr && t->opcode() == spv::Op::OpTypeFloat &&
                           t->NumInOperands() >= 1 && t->GetSingleWordInOperand(0) == 32;
                }

                // The value of a plain 32-bit OpConstant, or false (spec constants and anything
                // else make the caller decline rather than guess).
                bool PlainConstantValue(IRContext* ctx, uint32_t id, uint32_t& outValue) {
                    Instruction* def = ctx->get_def_use_mgr()->GetDef(id);
                    if (def == nullptr || def->opcode() != spv::Op::OpConstant) return false;
                    if (def->NumInOperands() != 1) return false;
                    outValue = def->GetSingleWordInOperand(0);
                    return true;
                }

                uint32_t Float32Type(IRContext* ctx) {
                    analysis::Float f(32);
                    return ctx->get_type_mgr()->GetTypeInstruction(&f);
                }

                // An OpTypeArray of float32 with the given length constant, reusing an existing
                // declaration when one exists.
                uint32_t ArrayOfFloat32Type(IRContext* ctx, uint32_t lengthConstId, uint32_t lengthValue) {
                    analysis::Float f(32);
                    analysis::Type* floatReg = ctx->get_type_mgr()->GetRegisteredType(&f);
                    const analysis::Array::LengthInfo lengthInfo{
                        lengthConstId,
                        {static_cast<uint32_t>(analysis::Array::LengthInfo::kConstant), lengthValue}};
                    analysis::Array arr(floatReg, lengthInfo);
                    return ctx->get_type_mgr()->GetTypeInstruction(&arr);
                }

                void AddNameFor(IRContext* ctx, uint32_t id, const String& name) {
                    std::vector<Operand> operands;
                    operands.push_back({SPV_OPERAND_TYPE_ID, {id}});
                    operands.push_back(
                        {SPV_OPERAND_TYPE_LITERAL_STRING, spvtools::utils::MakeVector(name)});
                    ctx->AddDebug2Inst(
                        spvtools::MakeUnique<Instruction>(ctx, spv::Op::OpName, 0, 0, operands));
                }

                void AddLocationDecoration(IRContext* ctx, uint32_t id, uint32_t location) {
                    ctx->AddAnnotationInst(spvtools::MakeUnique<Instruction>(
                        ctx, spv::Op::OpDecorate, 0, 0,
                        std::initializer_list<Operand>{
                            {SPV_OPERAND_TYPE_ID, {id}},
                            {SPV_OPERAND_TYPE_DECORATION,
                             {static_cast<uint32_t>(spv::Decoration::Location)}},
                            {SPV_OPERAND_TYPE_LITERAL_INTEGER, {location}}}));
                }

                // A fresh interface variable: declared, named, located, listed on the entry
                // point, and registered with the def-use manager so ReplaceAllUsesWith may name
                // it before the end-of-pass invalidation.
                uint32_t CreateCarrierVariable(IRContext* ctx, Instruction* entryPoint, uint32_t pointeeTypeId,
                                               spv::StorageClass storage, const String& name,
                                               uint32_t location) {
                    const uint32_t ptrTypeId = ctx->get_type_mgr()->FindPointerToType(pointeeTypeId, storage);
                    if (ptrTypeId == 0) return 0;
                    const uint32_t varId = ctx->TakeNextId();
                    auto var = spvtools::MakeUnique<Instruction>(
                        ctx, spv::Op::OpVariable, ptrTypeId, varId,
                        std::initializer_list<Operand>{
                            {SPV_OPERAND_TYPE_STORAGE_CLASS, {static_cast<uint32_t>(storage)}}});
                    Instruction* varInst = var.get();
                    ctx->AddGlobalValue(std::move(var));
                    ctx->get_def_use_mgr()->AnalyzeInstDefUse(varInst);
                    AddNameFor(ctx, varId, name);
                    AddLocationDecoration(ctx, varId, location);
                    entryPoint->AddOperand({SPV_OPERAND_TYPE_ID, {varId}});
                    return varId;
                }
            } // namespace

            spvtools::opt::Pass::Status DemotePointSizePass::Process() {
                auto* ctx = context();
                auto* defUse = ctx->get_def_use_mgr();
                const spv::ExecutionModel model = EntryExecutionModel(ctx);
                Instruction* entryPoint = EntryPoint(ctx);
                if (entryPoint == nullptr) return Status::SuccessWithoutChange;

                const bool isVertex = model == spv::ExecutionModel::Vertex;
                const bool isTessControl = model == spv::ExecutionModel::TessellationControl;
                const bool isTessEval = model == spv::ExecutionModel::TessellationEvaluation;
                const bool isGeometry = model == spv::ExecutionModel::Geometry;
                if (!isVertex && !isTessControl && !isTessEval && !isGeometry) {
                    return Status::SuccessWithoutChange;
                }

                const auto decline = [&](String reason) {
                    if (m_report != nullptr) {
                        m_report->declined = true;
                        m_report->declineReason = Move(reason);
                    }
                    return Status::SuccessWithoutChange;
                };

                // ---- discovery: where does PointSize live in this module ------------------
                // Member form: every struct type with a member decorated BuiltIn PointSize.
                struct MemberSite {
                    uint32_t structId = 0;
                    uint32_t memberIndex = 0;
                };
                std::vector<MemberSite> memberSites;
                // Standalone form: a variable decorated BuiltIn PointSize directly.
                std::vector<Instruction*> standaloneVars;
                std::vector<Instruction*> standaloneBuiltInDecorations;
                // Where clip and cull distance live, by (struct, member). DECLARATION is not
                // the question - glslang emits the whole four-member gl_PerVertex block into
                // every stage, touched or not - so these sites are only the starting point
                // for the ACCESS scan the control-stage decline below performs, which is the
                // same thing SPIRV-Cross's own clip_distance_count counts.
                std::vector<MemberSite> clipCullSites;
                std::vector<Instruction*> standaloneClipCullVars;
                const auto isClipOrCull = [](const uint32_t builtIn) {
                    return static_cast<spv::BuiltIn>(builtIn) == spv::BuiltIn::ClipDistance ||
                           static_cast<spv::BuiltIn>(builtIn) == spv::BuiltIn::CullDistance;
                };
                for (Instruction& ann : ctx->annotations()) {
                    if (ann.opcode() == spv::Op::OpMemberDecorate && ann.NumInOperands() >= 4 &&
                        static_cast<spv::Decoration>(ann.GetSingleWordInOperand(2)) ==
                            spv::Decoration::BuiltIn) {
                        if (static_cast<spv::BuiltIn>(ann.GetSingleWordInOperand(3)) ==
                            spv::BuiltIn::PointSize) {
                            memberSites.push_back(
                                {ann.GetSingleWordInOperand(0), ann.GetSingleWordInOperand(1)});
                        } else if (isClipOrCull(ann.GetSingleWordInOperand(3))) {
                            clipCullSites.push_back(
                                {ann.GetSingleWordInOperand(0), ann.GetSingleWordInOperand(1)});
                        }
                    } else if (ann.opcode() == spv::Op::OpDecorate && ann.NumInOperands() >= 3 &&
                               static_cast<spv::Decoration>(ann.GetSingleWordInOperand(1)) ==
                                   spv::Decoration::BuiltIn) {
                        if (static_cast<spv::BuiltIn>(ann.GetSingleWordInOperand(2)) ==
                            spv::BuiltIn::PointSize) {
                            Instruction* var = defUse->GetDef(ann.GetSingleWordInOperand(0));
                            if (var != nullptr && var->opcode() == spv::Op::OpVariable) {
                                standaloneVars.push_back(var);
                                standaloneBuiltInDecorations.push_back(&ann);
                            }
                        } else if (isClipOrCull(ann.GetSingleWordInOperand(2))) {
                            Instruction* var = defUse->GetDef(ann.GetSingleWordInOperand(0));
                            if (var != nullptr && var->opcode() == spv::Op::OpVariable) {
                                standaloneClipCullVars.push_back(var);
                            }
                        }
                    }
                }

                const auto memberIndexIn = [&](uint32_t structId, uint32_t& outMember) {
                    for (const MemberSite& site : memberSites) {
                        if (site.structId == structId) {
                            outMember = site.memberIndex;
                            return true;
                        }
                    }
                    return false;
                };

                // The gl_PerVertex-shaped interface variables: Input/Output variables whose
                // pointee is (an array of) a struct carrying a PointSize member.
                struct BlockVar {
                    Instruction* var = nullptr;
                    spv::StorageClass storage = spv::StorageClass::Output;
                    bool arrayed = false;
                    uint32_t arrayLengthConstId = 0;
                    uint32_t arrayLengthValue = 0;
                    uint32_t memberIndex = 0;
                };
                std::vector<BlockVar> blockVars;
                for (Instruction& inst : ctx->module()->types_values()) {
                    if (inst.opcode() != spv::Op::OpVariable) continue;
                    const auto storage = static_cast<spv::StorageClass>(inst.GetSingleWordInOperand(0));
                    if (storage != spv::StorageClass::Input && storage != spv::StorageClass::Output) {
                        continue;
                    }
                    uint32_t pointeeId = VariablePointeeType(ctx, &inst);
                    if (pointeeId == 0) continue;
                    Instruction* pointee = defUse->GetDef(pointeeId);
                    if (pointee == nullptr) continue;
                    BlockVar entry;
                    entry.var = &inst;
                    entry.storage = storage;
                    if (pointee->opcode() == spv::Op::OpTypeArray) {
                        entry.arrayed = true;
                        entry.arrayLengthConstId = pointee->GetSingleWordInOperand(1);
                        if (!PlainConstantValue(ctx, entry.arrayLengthConstId, entry.arrayLengthValue)) {
                            continue; // spec-constant-sized interface array: not glslang's shape
                        }
                        pointee = defUse->GetDef(pointee->GetSingleWordInOperand(0));
                        if (pointee == nullptr) continue;
                    }
                    if (pointee->opcode() != spv::Op::OpTypeStruct) continue;
                    if (!memberIndexIn(pointee->result_id(), entry.memberIndex)) continue;
                    blockVars.push_back(entry);
                }

                // ---- vertex stage: mirror, never demote -----------------------------------
                if (isVertex) {
                    if (!m_options.forceOutputCarrier) return Status::SuccessWithoutChange;
                    if (m_options.outputCarrierName.empty()) {
                        return decline("vertex mirror requested without a carrier name");
                    }
                    const uint32_t floatTypeId = Float32Type(ctx);
                    // The source of the mirrored value: the output block's PointSize member,
                    // a standalone output variable, or - with neither declared - the constant
                    // 1.0 GL's default point size names.
                    Instruction* blockVar = nullptr;
                    uint32_t memberIndex = 0;
                    for (const BlockVar& candidate : blockVars) {
                        if (candidate.storage == spv::StorageClass::Output && !candidate.arrayed) {
                            blockVar = candidate.var;
                            memberIndex = candidate.memberIndex;
                            break;
                        }
                    }
                    Instruction* standaloneOut = nullptr;
                    for (Instruction* candidate : standaloneVars) {
                        if (static_cast<spv::StorageClass>(candidate->GetSingleWordInOperand(0)) ==
                                spv::StorageClass::Output &&
                            IsFloat32Type(ctx, VariablePointeeType(ctx, candidate))) {
                            standaloneOut = candidate;
                            break;
                        }
                    }

                    const uint32_t carrierId =
                        CreateCarrierVariable(ctx, entryPoint, floatTypeId, spv::StorageClass::Output,
                                              m_options.outputCarrierName, m_options.location);
                    if (carrierId == 0) return decline("could not declare the vertex mirror carrier");

                    uint32_t memberConstId = 0;
                    uint32_t ptrOutputFloatId = 0;
                    if (blockVar != nullptr) {
                        memberConstId = ctx->get_constant_mgr()->GetSIntConstId(
                            static_cast<int32_t>(memberIndex));
                        ptrOutputFloatId =
                            ctx->get_type_mgr()->FindPointerToType(floatTypeId, spv::StorageClass::Output);
                        if (ptrOutputFloatId == 0) return decline("no Output float pointer type");
                    }
                    uint32_t defaultOneId = 0;
                    if (blockVar == nullptr && standaloneOut == nullptr) {
                        defaultOneId = ctx->get_constant_mgr()->GetFloatConstId(1.0f);
                    }

                    const uint32_t entryFunctionId = entryPoint->GetSingleWordInOperand(1);
                    bool mirrored = false;
                    for (auto funcIt = ctx->module()->begin(); funcIt != ctx->module()->end(); ++funcIt) {
                        if (funcIt->result_id() != entryFunctionId) continue;
                        funcIt->ForEachInst([&](Instruction* inst) {
                            if (inst->opcode() != spv::Op::OpReturn &&
                                inst->opcode() != spv::Op::OpReturnValue) {
                                return;
                            }
                            uint32_t valueId = 0;
                            if (blockVar != nullptr) {
                                const uint32_t chainId = ctx->TakeNextId();
                                inst->InsertBefore(spvtools::MakeUnique<Instruction>(
                                    ctx, spv::Op::OpAccessChain, ptrOutputFloatId, chainId,
                                    std::initializer_list<Operand>{
                                        {SPV_OPERAND_TYPE_ID, {blockVar->result_id()}},
                                        {SPV_OPERAND_TYPE_ID, {memberConstId}}}));
                                valueId = ctx->TakeNextId();
                                inst->InsertBefore(spvtools::MakeUnique<Instruction>(
                                    ctx, spv::Op::OpLoad, floatTypeId, valueId,
                                    std::initializer_list<Operand>{{SPV_OPERAND_TYPE_ID, {chainId}}}));
                            } else if (standaloneOut != nullptr) {
                                valueId = ctx->TakeNextId();
                                inst->InsertBefore(spvtools::MakeUnique<Instruction>(
                                    ctx, spv::Op::OpLoad, floatTypeId, valueId,
                                    std::initializer_list<Operand>{
                                        {SPV_OPERAND_TYPE_ID, {standaloneOut->result_id()}}}));
                            } else {
                                valueId = defaultOneId;
                            }
                            inst->InsertBefore(spvtools::MakeUnique<Instruction>(
                                ctx, spv::Op::OpStore, 0, 0,
                                std::initializer_list<Operand>{{SPV_OPERAND_TYPE_ID, {carrierId}},
                                                               {SPV_OPERAND_TYPE_ID, {valueId}}}));
                            mirrored = true;
                        });
                    }
                    if (!mirrored) {
                        // An entry function with no return is not a module glslang produces;
                        // the carrier stays declared (the consumer's read is undefined, as an
                        // unwritten built-in's would have been).
                    }
                    ctx->InvalidateAnalysesExceptFor(IRContext::kAnalysisNone);
                    return Status::SuccessWithChange;
                }

                // ---- tessellation / geometry: redirect and strip --------------------------
                // Phase 1: ANALYSIS ONLY. Every plan is collected before anything mutates, so
                // a decline leaves the module byte-identical.
                struct ArrayedRedirect {
                    Instruction* chain = nullptr;
                    bool input = false;
                };
                std::vector<ArrayedRedirect> arrayedRedirects; // gl_in[i].ps / gl_out[i].ps
                std::vector<Instruction*> scalarOutputChains;  // non-arrayed out block's member
                const BlockVar* arrayedInput = nullptr;
                const BlockVar* arrayedOutput = nullptr;

                for (const BlockVar& blockVar : blockVars) {
                    if (blockVar.arrayed) {
                        if (blockVar.storage == spv::StorageClass::Input) {
                            arrayedInput = &blockVar;
                        } else {
                            arrayedOutput = &blockVar;
                        }
                    }
                    bool declined = false;
                    String reason;
                    defUse->ForEachUser(blockVar.var, [&](Instruction* user) {
                        if (declined) return;
                        switch (user->opcode()) {
                        case spv::Op::OpEntryPoint:
                        case spv::Op::OpName:
                        case spv::Op::OpDecorate:
                            return;
                        case spv::Op::OpAccessChain:
                        case spv::Op::OpInBoundsAccessChain: {
                            const uint32_t indexCount = user->NumInOperands() - 1;
                            if (!blockVar.arrayed) {
                                if (indexCount < 1) {
                                    declined = true;
                                    reason = "an index-less pointer to the whole gl_PerVertex block";
                                    return;
                                }
                                uint32_t member = 0;
                                if (!PlainConstantValue(ctx, user->GetSingleWordInOperand(1), member)) {
                                    declined = true;
                                    reason = "a non-constant gl_PerVertex member index";
                                    return;
                                }
                                if (member != blockVar.memberIndex) return; // another member
                                if (indexCount != 1) {
                                    declined = true;
                                    reason = "an access chain that continues past the PointSize member";
                                    return;
                                }
                                scalarOutputChains.push_back(user);
                                return;
                            }
                            // Arrayed (gl_in / gl_out): [vertex, member, ...].
                            if (indexCount < 2) {
                                // A pointer that stops at the whole per-vertex struct can still
                                // reach PointSize through a second chain; following that split
                                // is not worth the shapes it would have to prove absent.
                                bool touchesPointSize = false;
                                defUse->ForEachUser(user, [&](Instruction* chainUser) {
                                    if ((chainUser->opcode() == spv::Op::OpAccessChain ||
                                         chainUser->opcode() == spv::Op::OpInBoundsAccessChain) &&
                                        chainUser->NumInOperands() >= 2) {
                                        uint32_t member = 0;
                                        if (PlainConstantValue(ctx, chainUser->GetSingleWordInOperand(1),
                                                               member) &&
                                            member == blockVar.memberIndex) {
                                            touchesPointSize = true;
                                        }
                                    } else if (chainUser->opcode() == spv::Op::OpLoad ||
                                               chainUser->opcode() == spv::Op::OpStore ||
                                               chainUser->opcode() == spv::Op::OpCopyMemory) {
                                        touchesPointSize = true; // whole-struct copy
                                    }
                                });
                                if (touchesPointSize) {
                                    declined = true;
                                    reason = "a split access chain or whole-struct copy reaching PointSize";
                                }
                                return;
                            }
                            uint32_t member = 0;
                            if (!PlainConstantValue(ctx, user->GetSingleWordInOperand(2), member)) {
                                declined = true;
                                reason = "a non-constant gl_PerVertex member index";
                                return;
                            }
                            if (member != blockVar.memberIndex) return; // another member
                            if (indexCount != 2) {
                                declined = true;
                                reason = "an access chain that continues past the PointSize member";
                                return;
                            }
                            arrayedRedirects.push_back(
                                {user, blockVar.storage == spv::StorageClass::Input});
                            return;
                        }
                        case spv::Op::OpLoad:
                        case spv::Op::OpStore:
                        case spv::Op::OpCopyMemory:
                            declined = true;
                            reason = "a whole-aggregate load/store/copy of the gl_PerVertex interface";
                            return;
                        default:
                            declined = true;
                            reason = std::format("SPIR-V opcode {} reaching the gl_PerVertex interface",
                                                 static_cast<uint32_t>(user->opcode()));
                            return;
                        }
                    });
                    if (declined) return decline(Move(reason));
                }

                // Standalone variables: swapping the decoration is only sound for the float /
                // float-array shapes the built-in is allowed to have; mixing forms in one
                // direction never comes out of glslang and declines.
                struct StandaloneSwap {
                    Instruction* var = nullptr;
                    Instruction* builtInDecoration = nullptr;
                    bool input = false;
                };
                std::vector<StandaloneSwap> standaloneSwaps;
                for (SizeT i = 0; i < standaloneVars.size(); ++i) {
                    Instruction* var = standaloneVars[i];
                    const auto storage = static_cast<spv::StorageClass>(var->GetSingleWordInOperand(0));
                    if (storage != spv::StorageClass::Input && storage != spv::StorageClass::Output) {
                        continue;
                    }
                    const bool input = storage == spv::StorageClass::Input;
                    uint32_t pointeeId = VariablePointeeType(ctx, var);
                    Instruction* pointee = defUse->GetDef(pointeeId);
                    if (pointee != nullptr && pointee->opcode() == spv::Op::OpTypeArray) {
                        pointee = defUse->GetDef(pointee->GetSingleWordInOperand(0));
                    }
                    if (pointee == nullptr || pointee->opcode() != spv::Op::OpTypeFloat) {
                        return decline("a standalone PointSize variable of an unexpected type");
                    }
                    if (input && arrayedInput != nullptr) {
                        return decline("PointSize declared both as a block member and standalone (input)");
                    }
                    if (!input && (arrayedOutput != nullptr || !scalarOutputChains.empty())) {
                        return decline("PointSize declared both as a block member and standalone (output)");
                    }
                    standaloneSwaps.push_back({var, standaloneBuiltInDecorations[i], input});
                }

                bool needsInputCarrier = false;
                bool needsOutputCarrier = m_options.forceOutputCarrier;
                for (const ArrayedRedirect& redirect : arrayedRedirects) {
                    (redirect.input ? needsInputCarrier : needsOutputCarrier) = true;
                    // Only a control stage has an ARRAYED output block; anywhere else this
                    // shape would hand a scalar carrier an extra index.
                    if (!redirect.input && !isTessControl) {
                        return decline("an arrayed PointSize output outside a control stage");
                    }
                }
                if (!scalarOutputChains.empty()) {
                    needsOutputCarrier = true;
                    // And only evaluation/geometry stages have the non-arrayed output block.
                    if (isTessControl) {
                        return decline("a non-arrayed PointSize output in a control stage");
                    }
                }
                bool standaloneInputSwapped = false;
                bool standaloneOutputSwapped = false;
                for (const StandaloneSwap& swap : standaloneSwaps) {
                    (swap.input ? standaloneInputSwapped : standaloneOutputSwapped) = true;
                }

                if (needsInputCarrier && arrayedInput == nullptr) {
                    return decline("a PointSize read with no arrayed input block to size the carrier by");
                }
                if (needsInputCarrier && m_options.inputCarrierName.empty()) {
                    return decline("a PointSize read with no input carrier name to bind it to");
                }
                if ((needsOutputCarrier && !standaloneOutputSwapped) &&
                    m_options.outputCarrierName.empty()) {
                    return decline("a PointSize write with no output carrier name to bind it to");
                }

                // A FORCED carrier with nothing redirected onto it is a carrier no instruction
                // ever writes - and a declared-but-unwritten output does not survive the ES
                // hop: the driver's GLSL front end drops it, and a transform-feedback request
                // naming it then fails the link with "varying undeclared", taking every other
                // capture in the set down with it. The pass therefore SEEDS such a carrier
                // (below) with GL's default point size, which is also what an unhosted
                // built-in rasterizes at. GL leaves the value of an unwritten output
                // undefined, so a defined 1.0 is a legal choice and a far better one than a
                // varying the driver deletes.
                const bool outputCarrierHasWriter = [&] {
                    if (!scalarOutputChains.empty()) return true;
                    for (const ArrayedRedirect& redirect : arrayedRedirects) {
                        if (!redirect.input) return true;
                    }
                    return false;
                }();
                // The seed the control stage would need is per-INVOCATION - gl_out[
                // gl_InvocationID] - and synthesizing that means inventing the InvocationId
                // built-in for a stage that may not declare it. A control stage asked to
                // produce a value it never computes is also a program reading undefined data
                // either way, so this declines rather than growing the pass for it.
                if (needsOutputCarrier && !standaloneOutputSwapped && !outputCarrierHasWriter &&
                    isTessControl) {
                    return decline("a control stage asked to produce a point-size carrier it never writes");
                }

                // The TCS output carrier is arrayed per vertex; its length comes from gl_out,
                // or - for a forced carrier in a control stage that never declared gl_out -
                // from the OutputVertices execution mode.
                uint32_t outputArrayLengthConstId = 0;
                uint32_t outputArrayLengthValue = 0;
                if (isTessControl && needsOutputCarrier && !standaloneOutputSwapped) {
                    if (arrayedOutput != nullptr) {
                        outputArrayLengthConstId = arrayedOutput->arrayLengthConstId;
                        outputArrayLengthValue = arrayedOutput->arrayLengthValue;
                    } else {
                        for (Instruction& mode : ctx->module()->execution_modes()) {
                            if (mode.NumInOperands() >= 3 &&
                                static_cast<spv::ExecutionMode>(mode.GetSingleWordInOperand(1)) ==
                                    spv::ExecutionMode::OutputVertices) {
                                outputArrayLengthValue = mode.GetSingleWordInOperand(2);
                                break;
                            }
                        }
                        if (outputArrayLengthValue == 0) {
                            return decline("a control stage with neither gl_out nor OutputVertices");
                        }
                        outputArrayLengthConstId =
                            ctx->get_constant_mgr()->GetUIntConstId(outputArrayLengthValue);
                    }
                }

                const bool anyWork = needsInputCarrier || needsOutputCarrier ||
                                     !standaloneSwaps.empty();
                // Even with no access left to redirect (a dead read the sanitize chain already
                // removed), a declared TessellationPointSize/GeometryPointSize capability must
                // still be stripped - it alone makes the module unbuildable on the device.
                std::vector<Instruction*> capabilitiesToStrip;
                for (Instruction& capability : ctx->module()->capabilities()) {
                    if (capability.NumInOperands() < 1) continue;
                    const auto declared =
                        static_cast<spv::Capability>(capability.GetSingleWordInOperand(0));
                    if (declared == spv::Capability::TessellationPointSize ||
                        declared == spv::Capability::GeometryPointSize) {
                        capabilitiesToStrip.push_back(&capability);
                    }
                }
                if (!anyWork && capabilitiesToStrip.empty()) return Status::SuccessWithoutChange;

                // THE ONE SHAPE WHERE "declared but unaccessed" IS NOT ENOUGH. SPIRV-Cross
                // force-emits the whole redeclared gl_PerVertex OUTPUT block for a control
                // stage whose clip or cull distances are LIVE (should_force_emit_builtin_block,
                // spirv_glsl.cpp), and that emission is driven by the struct's member
                // DECORATIONS, never by access - so it prints "float gl_PointSize;" into a
                // block no instruction touches any more. On the extension-less ES drivers this
                // pass exists for, that redeclaration is exactly as illegal as the access was,
                // so the demoted program would still be lost - only now with the capability
                // stripped, the program-wide verdict flipped and a diagnostic naming a
                // built-in the module no longer mentions. Declining keeps the honest refusal,
                // and keeps the header's claim true for every shape that IS demoted.
                //
                // LIVE, not declared: glslang emits the whole four-member gl_PerVertex block
                // into every stage whether or not it is touched, and SPIRV-Cross counts clip
                // and cull distance from ACCESSES (Compiler::ActiveBuiltinHandler). Keying
                // this on the decorations alone would decline every control stage there is.
                // Only a block-MEMBER PointSize can be left behind to be printed; a standalone
                // variable leaves with the demotion.
                if (isTessControl && !blockVars.empty()) {
                    bool clipOrCullIsLive = false;
                    const auto memberIsClipOrCull = [&](const uint32_t structId,
                                                        const uint32_t member) {
                        for (const MemberSite& site : clipCullSites) {
                            if (site.structId == structId && site.memberIndex == member) return true;
                        }
                        return false;
                    };
                    for (Instruction& inst : ctx->module()->types_values()) {
                        if (clipOrCullIsLive) break;
                        if (inst.opcode() != spv::Op::OpVariable) continue;
                        const auto storage =
                            static_cast<spv::StorageClass>(inst.GetSingleWordInOperand(0));
                        if (storage != spv::StorageClass::Input &&
                            storage != spv::StorageClass::Output) {
                            continue;
                        }
                        // A standalone clip/cull variable counts the moment anything but its
                        // own declaration touches it.
                        bool standaloneClipCull = false;
                        for (Instruction* candidate : standaloneClipCullVars) {
                            if (candidate == &inst) standaloneClipCull = true;
                        }
                        Instruction* pointee = defUse->GetDef(VariablePointeeType(ctx, &inst));
                        bool arrayed = false;
                        if (pointee != nullptr && pointee->opcode() == spv::Op::OpTypeArray) {
                            arrayed = true;
                            pointee = defUse->GetDef(pointee->GetSingleWordInOperand(0));
                        }
                        const bool blockCarriesClipCull =
                            pointee != nullptr && pointee->opcode() == spv::Op::OpTypeStruct &&
                            [&] {
                                for (const MemberSite& site : clipCullSites) {
                                    if (site.structId == pointee->result_id()) return true;
                                }
                                return false;
                            }();
                        if (!standaloneClipCull && !blockCarriesClipCull) continue;
                        const uint32_t structId = blockCarriesClipCull ? pointee->result_id() : 0;
                        const uint32_t memberOperand = arrayed ? 2u : 1u;
                        defUse->ForEachUser(&inst, [&](Instruction* user) {
                            if (clipOrCullIsLive) return;
                            switch (user->opcode()) {
                            case spv::Op::OpEntryPoint:
                            case spv::Op::OpName:
                            case spv::Op::OpMemberName:
                            case spv::Op::OpDecorate:
                            case spv::Op::OpMemberDecorate:
                                return;
                            default:
                                break;
                            }
                            if (standaloneClipCull) {
                                clipOrCullIsLive = true;
                                return;
                            }
                            if (user->opcode() != spv::Op::OpAccessChain &&
                                user->opcode() != spv::Op::OpInBoundsAccessChain) {
                                // A whole-block load, copy or anything else that cannot be
                                // narrowed reaches every member, clip distance included.
                                clipOrCullIsLive = true;
                                return;
                            }
                            uint32_t member = 0;
                            if (user->NumInOperands() <= memberOperand ||
                                !PlainConstantValue(ctx, user->GetSingleWordInOperand(memberOperand),
                                                    member)) {
                                clipOrCullIsLive = true; // cannot prove it misses clip/cull
                                return;
                            }
                            if (memberIsClipOrCull(structId, member)) clipOrCullIsLive = true;
                        });
                    }
                    if (clipOrCullIsLive) {
                        for (const BlockVar& blockVar : blockVars) {
                            if (blockVar.storage == spv::StorageClass::Output) {
                                return decline(
                                    "a control stage with live clip/cull distance, whose "
                                    "redeclared output block would still print gl_PointSize");
                            }
                        }
                    }
                }

                // Phase 2: MUTATION. Nothing below may decline.
                const uint32_t floatTypeId = Float32Type(ctx);
                uint32_t inputCarrierId = 0;
                if (needsInputCarrier) {
                    const uint32_t arrayTypeId = ArrayOfFloat32Type(
                        ctx, arrayedInput->arrayLengthConstId, arrayedInput->arrayLengthValue);
                    inputCarrierId =
                        CreateCarrierVariable(ctx, entryPoint, arrayTypeId, spv::StorageClass::Input,
                                              m_options.inputCarrierName, m_options.location);
                }
                uint32_t outputCarrierId = 0;
                if (needsOutputCarrier && !standaloneOutputSwapped) {
                    uint32_t pointeeTypeId = floatTypeId;
                    if (isTessControl) {
                        pointeeTypeId =
                            ArrayOfFloat32Type(ctx, outputArrayLengthConstId, outputArrayLengthValue);
                    }
                    outputCarrierId =
                        CreateCarrierVariable(ctx, entryPoint, pointeeTypeId, spv::StorageClass::Output,
                                              m_options.outputCarrierName, m_options.location);
                }

                // Seed a forced carrier nothing writes, so the ES hop keeps it (see the
                // reasoning at outputCarrierHasWriter). In a GEOMETRY stage the store has to
                // go before every EmitVertex, because that is when the outputs of one vertex
                // are latched; anywhere else the ends of the entry function will do.
                if (outputCarrierId != 0 && !outputCarrierHasWriter) {
                    const uint32_t defaultPointSizeId = ctx->get_constant_mgr()->GetFloatConstId(1.0f);
                    const uint32_t entryFunctionId = entryPoint->GetSingleWordInOperand(1);
                    std::vector<Instruction*> seedSites;
                    for (auto funcIt = ctx->module()->begin(); funcIt != ctx->module()->end();
                         ++funcIt) {
                        if (funcIt->result_id() != entryFunctionId) continue;
                        funcIt->ForEachInst([&](Instruction* inst) {
                            const bool emit = inst->opcode() == spv::Op::OpEmitVertex ||
                                              inst->opcode() == spv::Op::OpEmitStreamVertex;
                            const bool ret = inst->opcode() == spv::Op::OpReturn ||
                                             inst->opcode() == spv::Op::OpReturnValue;
                            if (isGeometry ? emit : ret) seedSites.push_back(inst);
                        });
                        // A geometry stage with no EmitVertex emits nothing at all; seeding the
                        // ends of the function still keeps the varying alive for the capture.
                        if (isGeometry && seedSites.empty()) {
                            funcIt->ForEachInst([&](Instruction* inst) {
                                if (inst->opcode() == spv::Op::OpReturn ||
                                    inst->opcode() == spv::Op::OpReturnValue) {
                                    seedSites.push_back(inst);
                                }
                            });
                        }
                    }
                    for (Instruction* site : seedSites) {
                        site->InsertBefore(spvtools::MakeUnique<Instruction>(
                            ctx, spv::Op::OpStore, 0, 0,
                            std::initializer_list<Operand>{
                                {SPV_OPERAND_TYPE_ID, {outputCarrierId}},
                                {SPV_OPERAND_TYPE_ID, {defaultPointSizeId}}}));
                    }
                }

                // Scalar output chains first, while the def-use index still knows their uses.
                for (Instruction* chain : scalarOutputChains) {
                    ctx->ReplaceAllUsesWith(chain->result_id(), outputCarrierId);
                    ctx->KillInst(chain);
                }
                // Arrayed chains are rewritten in place: same result id, same result type
                // (pointer-to-float in the same storage class), one fewer index.
                for (const ArrayedRedirect& redirect : arrayedRedirects) {
                    const uint32_t carrierId = redirect.input ? inputCarrierId : outputCarrierId;
                    const Operand vertexIndex = redirect.chain->GetInOperand(1);
                    redirect.chain->SetInOperands(Instruction::OperandList{
                        {SPV_OPERAND_TYPE_ID, {carrierId}}, vertexIndex});
                }
                // Standalone form: the variable becomes its own carrier.
                for (const StandaloneSwap& swap : standaloneSwaps) {
                    ctx->KillInst(swap.builtInDecoration);
                    AddLocationDecoration(ctx, swap.var->result_id(), m_options.location);
                    std::vector<Instruction*> oldNames;
                    for (Instruction& debugInst : ctx->module()->debugs2()) {
                        if (debugInst.opcode() == spv::Op::OpName &&
                            debugInst.GetSingleWordInOperand(0) == swap.var->result_id()) {
                            oldNames.push_back(&debugInst);
                        }
                    }
                    for (Instruction* oldName : oldNames) ctx->KillInst(oldName);
                    AddNameFor(ctx, swap.var->result_id(),
                               swap.input ? m_options.inputCarrierName : m_options.outputCarrierName);
                }
                for (Instruction* capability : capabilitiesToStrip) {
                    ctx->KillInst(capability);
                }

                if (m_report != nullptr) {
                    m_report->createdInputCarrier = needsInputCarrier || standaloneInputSwapped;
                }
                ctx->InvalidateAnalysesExceptFor(IRContext::kAnalysisNone);
                return Status::SuccessWithChange;
            }

            spvtools::Optimizer::PassToken DemotePointSizePass::CreateDemotePointSizePass(
                DemotePointSizeOptions options, DemotePointSizeReport* report) {
                return spvtools::Optimizer::PassToken(
                    MakeUnique<DemotePointSizePass>(Move(options), report));
            }
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
