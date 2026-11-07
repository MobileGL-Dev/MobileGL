// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/EmulateClipDistancePass.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "EmulateClipDistancePass.h"

#include "spirv.hpp"
#include "source/opt/basic_block.h"
#include "source/opt/build_module.h"
#include "source/opt/constants.h"
#include "source/opt/def_use_manager.h"
#include "source/opt/function.h"
#include "source/opt/instruction.h"
#include "source/opt/ir_builder.h"
#include "source/opt/ir_context.h"
#include "source/opt/module.h"
#include "source/opt/type_manager.h"
#include "source/opt/types.h"
#include "source/util/make_unique.h"
#include "source/util/string_utils.h"

#include <format>
#include <unordered_set>
#include <vector>

namespace MobileGL {
    namespace MG_Util {
        namespace ShaderTranspiler {
            namespace {
                using spvtools::opt::BasicBlock;
                using spvtools::opt::Function;
                using spvtools::opt::Instruction;
                using spvtools::opt::InstructionBuilder;
                using spvtools::opt::IRContext;
                using spvtools::opt::Operand;
                namespace analysis = spvtools::opt::analysis;

                const IRContext::Analysis kBuilderAnalyses = static_cast<IRContext::Analysis>(
                    IRContext::kAnalysisDefUse | IRContext::kAnalysisInstrToBlockMapping);

                Instruction* EntryPoint(IRContext* ctx) {
                    for (Instruction& ep : ctx->module()->entry_points()) {
                        return &ep;
                    }
                    return nullptr;
                }

                Function* EntryFunction(IRContext* ctx, Instruction* entryPoint) {
                    const uint32_t id = entryPoint->GetSingleWordInOperand(1);
                    for (auto& function : *ctx->module()) {
                        if (function.result_id() == id) return &function;
                    }
                    return nullptr;
                }

                uint32_t PointeeOf(IRContext* ctx, uint32_t pointerTypeId) {
                    Instruction* ptrType = ctx->get_def_use_mgr()->GetDef(pointerTypeId);
                    if (ptrType == nullptr || ptrType->opcode() != spv::Op::OpTypePointer) return 0;
                    return ptrType->GetSingleWordInOperand(1);
                }

                bool PlainConstantValue(IRContext* ctx, uint32_t id, uint32_t& outValue) {
                    Instruction* def = ctx->get_def_use_mgr()->GetDef(id);
                    if (def == nullptr || def->opcode() != spv::Op::OpConstant) return false;
                    if (def->NumInOperands() != 1) return false;
                    outValue = def->GetSingleWordInOperand(0);
                    return true;
                }

                // Length of an OpTypeArray of 32-bit float with a plain constant length, else 0.
                uint32_t FloatArrayLength(IRContext* ctx, uint32_t typeId) {
                    Instruction* t = ctx->get_def_use_mgr()->GetDef(typeId);
                    if (t == nullptr || t->opcode() != spv::Op::OpTypeArray) return 0;
                    Instruction* element = ctx->get_def_use_mgr()->GetDef(t->GetSingleWordInOperand(0));
                    if (element == nullptr || element->opcode() != spv::Op::OpTypeFloat ||
                        element->GetSingleWordInOperand(0) != 32) {
                        return 0;
                    }
                    uint32_t length = 0;
                    if (!PlainConstantValue(ctx, t->GetSingleWordInOperand(1), length)) return 0;
                    return length;
                }

                void AddNameFor(IRContext* ctx, uint32_t id, const String& name) {
                    std::vector<Operand> operands;
                    operands.push_back({SPV_OPERAND_TYPE_ID, {id}});
                    operands.push_back({SPV_OPERAND_TYPE_LITERAL_STRING, spvtools::utils::MakeVector(name)});
                    ctx->AddDebug2Inst(spvtools::MakeUnique<Instruction>(ctx, spv::Op::OpName, 0, 0, operands));
                }

                uint32_t AddGlobalVariable(IRContext* ctx, uint32_t pointeeTypeId, spv::StorageClass storage,
                                           const String& name) {
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
                    return varId;
                }

                void AddLocationDecoration(IRContext* ctx, uint32_t id, uint32_t location) {
                    ctx->AddAnnotationInst(spvtools::MakeUnique<Instruction>(
                        ctx, spv::Op::OpDecorate, 0, 0,
                        std::initializer_list<Operand>{
                            {SPV_OPERAND_TYPE_ID, {id}},
                            {SPV_OPERAND_TYPE_DECORATION, {static_cast<uint32_t>(spv::Decoration::Location)}},
                            {SPV_OPERAND_TYPE_LITERAL_INTEGER, {location}}}));
                }

                void RemoveFromInterface(Instruction* entryPoint, uint32_t id) {
                    Instruction::OperandList kept;
                    for (uint32_t i = 0; i < entryPoint->NumInOperands(); ++i) {
                        const Operand& operand = entryPoint->GetInOperand(i);
                        if (i >= 3 && operand.type == SPV_OPERAND_TYPE_ID && operand.words.size() == 1 &&
                            operand.words[0] == id) {
                            continue;
                        }
                        kept.push_back(operand);
                    }
                    entryPoint->SetInOperands(Move(kept));
                }

                // Whether every use of a pointer into the built-in is one the retype below can
                // follow: loads, stores (as the POINTER), copies, and further access chains.
                bool PointerUsesAreRetypable(IRContext* ctx, Instruction* pointer, String& reason) {
                    bool ok = true;
                    ctx->get_def_use_mgr()->ForEachUse(pointer, [&](Instruction* user, uint32_t operandIndex) {
                        if (!ok) return;
                        switch (user->opcode()) {
                        case spv::Op::OpName:
                        case spv::Op::OpDecorate:
                        case spv::Op::OpEntryPoint:
                            return;
                        case spv::Op::OpLoad:
                        case spv::Op::OpCopyMemory:
                            return;
                        case spv::Op::OpStore:
                            // Operand 0 of OpStore is the pointer; anything else would be
                            // storing the pointer itself, which logical addressing forbids.
                            if (operandIndex != 0) {
                                ok = false;
                                reason = "a pointer into gl_ClipDistance stored as a value";
                            }
                            return;
                        case spv::Op::OpAccessChain:
                        case spv::Op::OpInBoundsAccessChain:
                            // Base operand: the instruction's first id after result type/id.
                            if (operandIndex != 2) {
                                ok = false;
                                reason = "a pointer into gl_ClipDistance used as an access-chain index";
                                return;
                            }
                            if (!PointerUsesAreRetypable(ctx, user, reason)) ok = false;
                            return;
                        default:
                            ok = false;
                            reason = std::format("a pointer into gl_ClipDistance reaching SPIR-V opcode {}",
                                                 static_cast<uint32_t>(user->opcode()));
                            return;
                        }
                    });
                    return ok;
                }

                // Re-points every access chain hanging off `pointer` (which now lives in Private
                // storage) at a Private pointer type of the same pointee.
                void RetypeChainUsers(IRContext* ctx, Instruction* pointer) {
                    std::vector<Instruction*> chains;
                    ctx->get_def_use_mgr()->ForEachUser(pointer, [&](Instruction* user) {
                        if ((user->opcode() == spv::Op::OpAccessChain ||
                             user->opcode() == spv::Op::OpInBoundsAccessChain) &&
                            user->GetSingleWordInOperand(0) == pointer->result_id()) {
                            chains.push_back(user);
                        }
                    });
                    for (Instruction* chain : chains) {
                        const uint32_t pointee = PointeeOf(ctx, chain->type_id());
                        chain->SetResultType(
                            ctx->get_type_mgr()->FindPointerToType(pointee, spv::StorageClass::Private));
                        ctx->get_def_use_mgr()->AnalyzeInstUse(chain);
                        RetypeChainUsers(ctx, chain);
                    }
                }

                // A scalar constant of the given 32-bit integer type.
                uint32_t IntConstantOfType(IRContext* ctx, uint32_t intTypeId, uint32_t value) {
                    const analysis::Type* type = ctx->get_type_mgr()->GetType(intTypeId);
                    const analysis::Integer* integer = type != nullptr ? type->AsInteger() : nullptr;
                    if (integer != nullptr && integer->IsSigned()) {
                        return ctx->get_constant_mgr()->GetSIntConstId(static_cast<int32_t>(value));
                    }
                    return ctx->get_constant_mgr()->GetUIntConstId(value);
                }

                bool IsInt32Type(IRContext* ctx, uint32_t typeId) {
                    const analysis::Type* type = ctx->get_type_mgr()->GetType(typeId);
                    const analysis::Integer* integer = type != nullptr ? type->AsInteger() : nullptr;
                    return integer != nullptr && integer->width() == 32;
                }

                struct ClipSites {
                    // struct type id -> member index of its BuiltIn ClipDistance member
                    std::vector<std::pair<uint32_t, uint32_t>> members;
                    std::unordered_set<uint32_t> standaloneVars;
                    Instruction* capability = nullptr;
                };

                ClipSites FindClipSites(IRContext* ctx) {
                    ClipSites sites;
                    for (Instruction& ann : ctx->annotations()) {
                        if (ann.opcode() == spv::Op::OpMemberDecorate && ann.NumInOperands() >= 4 &&
                            static_cast<spv::Decoration>(ann.GetSingleWordInOperand(2)) == spv::Decoration::BuiltIn &&
                            static_cast<spv::BuiltIn>(ann.GetSingleWordInOperand(3)) == spv::BuiltIn::ClipDistance) {
                            sites.members.emplace_back(ann.GetSingleWordInOperand(0), ann.GetSingleWordInOperand(1));
                        } else if (ann.opcode() == spv::Op::OpDecorate && ann.NumInOperands() >= 3 &&
                                   static_cast<spv::Decoration>(ann.GetSingleWordInOperand(1)) ==
                                       spv::Decoration::BuiltIn &&
                                   static_cast<spv::BuiltIn>(ann.GetSingleWordInOperand(2)) ==
                                       spv::BuiltIn::ClipDistance) {
                            Instruction* var = ctx->get_def_use_mgr()->GetDef(ann.GetSingleWordInOperand(0));
                            if (var != nullptr && var->opcode() == spv::Op::OpVariable) {
                                sites.standaloneVars.insert(var->result_id());
                            }
                        }
                    }
                    for (Instruction& capability : ctx->module()->capabilities()) {
                        if (capability.NumInOperands() >= 1 &&
                            static_cast<spv::Capability>(capability.GetSingleWordInOperand(0)) ==
                                spv::Capability::ClipDistance) {
                            sites.capability = &capability;
                        }
                    }
                    return sites;
                }

                bool ClipMemberOf(const ClipSites& sites, uint32_t structId, uint32_t& member) {
                    for (const auto& [id, index] : sites.members) {
                        if (id == structId) {
                            member = index;
                            return true;
                        }
                    }
                    return false;
                }
            } // namespace

            ClipDistanceModuleProbe EmulateClipDistancePass::ProbeModule(const Vector<Uint32>& spirv) {
                ClipDistanceModuleProbe probe;
                if (spirv.empty()) {
                    probe.parsed = true;
                    return probe;
                }
                std::unique_ptr<IRContext> ctx = spvtools::BuildModule(
                    SPV_ENV_VULKAN_1_1, [](spv_message_level_t, const char*, const spv_position_t&, const char*) {},
                    spirv.data(), spirv.size());
                if (!ctx) return probe;
                probe.parsed = true;
                const ClipSites sites = FindClipSites(ctx.get());
                probe.declaresClipDistanceCapability = sites.capability != nullptr;
                for (Instruction& inst : ctx->module()->types_values()) {
                    if (inst.opcode() != spv::Op::OpVariable) continue;
                    if (static_cast<spv::StorageClass>(inst.GetSingleWordInOperand(0)) != spv::StorageClass::Output) {
                        continue;
                    }
                    const uint32_t pointee = PointeeOf(ctx.get(), inst.type_id());
                    if (sites.standaloneVars.count(inst.result_id()) != 0) {
                        probe.outputDistanceCount = FloatArrayLength(ctx.get(), pointee);
                        break;
                    }
                    Instruction* type = ctx->get_def_use_mgr()->GetDef(pointee);
                    uint32_t member = 0;
                    if (type != nullptr && type->opcode() == spv::Op::OpTypeStruct &&
                        ClipMemberOf(sites, type->result_id(), member)) {
                        probe.outputDistanceCount = FloatArrayLength(ctx.get(), type->GetSingleWordInOperand(member));
                        break;
                    }
                }
                return probe;
            }

            spvtools::opt::Pass::Status EmulateClipDistancePass::Process() {
                IRContext* ctx = context();
                auto* defUse = ctx->get_def_use_mgr();
                auto* typeMgr = ctx->get_type_mgr();
                auto* constMgr = ctx->get_constant_mgr();
                using Role = EmulateClipDistanceOptions::Role;

                Instruction* entryPoint = EntryPoint(ctx);
                if (entryPoint == nullptr) return Status::SuccessWithoutChange;
                const auto model = static_cast<spv::ExecutionModel>(entryPoint->GetSingleWordInOperand(0));
                const bool isFragment = model == spv::ExecutionModel::Fragment;
                const bool isGeometry = model == spv::ExecutionModel::Geometry;
                const bool isPreRaster = model == spv::ExecutionModel::Vertex ||
                                         model == spv::ExecutionModel::TessellationControl ||
                                         model == spv::ExecutionModel::TessellationEvaluation || isGeometry;

                const auto decline = [&](String reason) {
                    if (m_report != nullptr) {
                        m_report->declined = true;
                        m_report->declineReason = Move(reason);
                    }
                    return Status::SuccessWithoutChange;
                };
                if (m_options.role == Role::Consumer && !isFragment) return decline("consumer role on a non-fragment stage");
                if (m_options.role != Role::Consumer && !isPreRaster) {
                    return decline("producer/redirect role on a non-pre-rasterization stage");
                }
                if (m_options.role == Role::Producer && model == spv::ExecutionModel::TessellationControl) {
                    return decline("a control stage cannot be the last pre-rasterization stage");
                }
                const bool native = m_options.nativeBuiltin;
                if (native && m_options.role != Role::Producer) return decline("native masking outside the producer");

                const ClipSites sites = FindClipSites(ctx);

                // ---- Phase 1: ANALYSIS ONLY. A decline leaves the module byte-identical. ----
                std::vector<Instruction*> memberChains;    // [clipMember, rest...] on a non-arrayed block
                std::vector<Instruction*> standaloneVars;  // standalone ClipDistance variables
                // Native masking: where the masked values are written back to.
                Instruction* nativeBlockVar = nullptr;
                uint32_t nativeBlockMember = 0;
                Instruction* nativeStandaloneVar = nullptr;
                uint32_t shadowTypeId = 0;
                String reason;
                const auto noteShadowType = [&](uint32_t typeId) {
                    if (shadowTypeId == 0) {
                        shadowTypeId = typeId;
                        return true;
                    }
                    return shadowTypeId == typeId;
                };

                for (Instruction& inst : ctx->module()->types_values()) {
                    if (inst.opcode() != spv::Op::OpVariable) continue;
                    const auto storage = static_cast<spv::StorageClass>(inst.GetSingleWordInOperand(0));
                    if (storage != spv::StorageClass::Input && storage != spv::StorageClass::Output) continue;
                    // Native masking leaves every INPUT alone: gl_in[] reads are hosted natively.
                    if (native && storage == spv::StorageClass::Input) continue;
                    const uint32_t pointeeId = PointeeOf(ctx, inst.type_id());

                    if (sites.standaloneVars.count(inst.result_id()) != 0) {
                        if (native) nativeStandaloneVar = &inst;
                        if (FloatArrayLength(ctx, pointeeId) == 0) {
                            return decline("a standalone gl_ClipDistance that is not a sized float array");
                        }
                        // A pre-rasterization stage's standalone INPUT would be gl_in data,
                        // which a non-arrayed variable cannot be; only the fragment stage reads
                        // gl_ClipDistance this way.
                        if (storage == spv::StorageClass::Input && !isFragment) {
                            return decline("a standalone gl_ClipDistance input outside the fragment stage");
                        }
                        if (!noteShadowType(pointeeId)) return decline("two differently sized gl_ClipDistance arrays");
                        if (!PointerUsesAreRetypable(ctx, &inst, reason)) return decline(Move(reason));
                        standaloneVars.push_back(&inst);
                        continue;
                    }

                    Instruction* pointee = defUse->GetDef(pointeeId);
                    if (pointee == nullptr) continue;
                    bool arrayed = false;
                    if (pointee->opcode() == spv::Op::OpTypeArray) {
                        arrayed = true;
                        pointee = defUse->GetDef(pointee->GetSingleWordInOperand(0));
                        if (pointee == nullptr) continue;
                    }
                    uint32_t clipMember = 0;
                    if (pointee->opcode() != spv::Op::OpTypeStruct || !ClipMemberOf(sites, pointee->result_id(), clipMember)) {
                        continue;
                    }
                    const uint32_t memberTypeId = pointee->GetSingleWordInOperand(clipMember);
                    const uint32_t memberOperand = arrayed ? 2u : 1u; // [vertex,] member, ...
                    if (native && !arrayed && storage == spv::StorageClass::Output) {
                        nativeBlockVar = &inst;
                        nativeBlockMember = clipMember;
                    }
                    bool declined = false;
                    defUse->ForEachUser(&inst, [&](Instruction* user) {
                        if (declined) return;
                        switch (user->opcode()) {
                        case spv::Op::OpEntryPoint:
                        case spv::Op::OpName:
                        case spv::Op::OpDecorate:
                            return;
                        case spv::Op::OpAccessChain:
                        case spv::Op::OpInBoundsAccessChain: {
                            if (user->NumInOperands() <= memberOperand) {
                                // Stops short of the member: a whole block (or gl_in[i])
                                // pointer. Harmless unless something then reaches the clip
                                // member through it or copies the whole struct.
                                bool touches = false;
                                defUse->ForEachUser(user, [&](Instruction* next) {
                                    if (next->opcode() == spv::Op::OpAccessChain ||
                                        next->opcode() == spv::Op::OpInBoundsAccessChain) {
                                        uint32_t member = 0;
                                        if (next->NumInOperands() < 2 ||
                                            !PlainConstantValue(ctx, next->GetSingleWordInOperand(1), member) ||
                                            member == clipMember) {
                                            touches = true;
                                        }
                                    } else if (next->opcode() != spv::Op::OpName) {
                                        touches = true; // whole-struct load/store/copy
                                    }
                                });
                                if (touches) {
                                    declined = true;
                                    reason = "a split access chain or whole-gl_PerVertex copy reaching gl_ClipDistance";
                                }
                                return;
                            }
                            uint32_t member = 0;
                            if (!PlainConstantValue(ctx, user->GetSingleWordInOperand(memberOperand), member)) {
                                declined = true;
                                reason = "a non-constant gl_PerVertex member index";
                                return;
                            }
                            if (member != clipMember) return; // another member
                            if (arrayed) {
                                declined = true;
                                reason = storage == spv::StorageClass::Input
                                             ? "a gl_in[].gl_ClipDistance read"
                                             : "a gl_out[].gl_ClipDistance write";
                                return;
                            }
                            if (!noteShadowType(memberTypeId)) {
                                declined = true;
                                reason = "two differently sized gl_ClipDistance arrays";
                                return;
                            }
                            if (!PointerUsesAreRetypable(ctx, user, reason)) {
                                declined = true;
                                return;
                            }
                            memberChains.push_back(user);
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

                const bool hasAccesses = !memberChains.empty() || !standaloneVars.empty();
                if (shadowTypeId == 0 && m_options.role == Role::Producer) {
                    // A producer with nothing redirected may still have declared the carrier
                    // length the driver planned with; take it from its Output declaration.
                    for (Instruction& inst : ctx->module()->types_values()) {
                        if (inst.opcode() != spv::Op::OpVariable ||
                            static_cast<spv::StorageClass>(inst.GetSingleWordInOperand(0)) != spv::StorageClass::Output) {
                            continue;
                        }
                        Instruction* type = defUse->GetDef(PointeeOf(ctx, inst.type_id()));
                        uint32_t member = 0;
                        if (type != nullptr && type->opcode() == spv::Op::OpTypeStruct &&
                            ClipMemberOf(sites, type->result_id(), member)) {
                            shadowTypeId = type->GetSingleWordInOperand(member);
                            break;
                        }
                    }
                }
                const uint32_t shadowLength = shadowTypeId != 0 ? FloatArrayLength(ctx, shadowTypeId) : 0;
                if (shadowTypeId != 0 && shadowLength == 0) return decline("gl_ClipDistance is not a sized float array");

                Uint32 producedCount = 0;
                if (m_options.role == Role::Producer) {
                    producedCount = std::min<Uint32>(shadowLength, kMaxCarriers * 4);
                    if (shadowLength > kMaxCarriers * 4) return decline("more than eight clip distances");
                }
                const Uint32 consumedCount =
                    m_options.role == Role::Consumer ? std::min<Uint32>(m_options.distanceCount, kMaxCarriers * 4) : 0;

                // The producer's enables uniform: a member of a Uniform-storage struct, by name.
                uint32_t enablesVarId = 0;
                uint32_t enablesMemberIndex = 0;
                uint32_t enablesTypeId = 0;
                if (producedCount > 0 && !m_options.enablesMemberName.empty()) {
                    for (Instruction& debug : ctx->module()->debugs2()) {
                        if (enablesVarId != 0) break;
                        if (debug.opcode() != spv::Op::OpMemberName || debug.NumInOperands() < 3) continue;
                        if (debug.GetOperand(2).AsString() != m_options.enablesMemberName) continue;
                        const uint32_t structId = debug.GetSingleWordInOperand(0);
                        const uint32_t member = debug.GetSingleWordInOperand(1);
                        Instruction* structType = defUse->GetDef(structId);
                        if (structType == nullptr || structType->opcode() != spv::Op::OpTypeStruct ||
                            member >= structType->NumInOperands()) {
                            continue;
                        }
                        const uint32_t memberType = structType->GetSingleWordInOperand(member);
                        if (!IsInt32Type(ctx, memberType)) continue;
                        for (Instruction& var : ctx->module()->types_values()) {
                            if (var.opcode() != spv::Op::OpVariable ||
                                static_cast<spv::StorageClass>(var.GetSingleWordInOperand(0)) !=
                                    spv::StorageClass::Uniform ||
                                PointeeOf(ctx, var.type_id()) != structId) {
                                continue;
                            }
                            enablesVarId = var.result_id();
                            enablesMemberIndex = member;
                            enablesTypeId = memberType;
                            break;
                        }
                    }
                }

                if (!hasAccesses && sites.capability == nullptr && producedCount == 0 && consumedCount == 0) {
                    return Status::SuccessWithoutChange;
                }
                if (native && producedCount > 0 && nativeBlockVar == nullptr && nativeStandaloneVar == nullptr) {
                    return decline("no gl_ClipDistance output to write the masked distances back into");
                }

                // ---- Phase 2: MUTATION. Nothing below may decline. ----
                const bool interfaceListsAllGlobals = ctx->module()->version() >= 0x00010400u;
                uint32_t shadowId = 0;
                if (shadowTypeId != 0 && (hasAccesses || producedCount > 0)) {
                    shadowId = AddGlobalVariable(ctx, shadowTypeId, spv::StorageClass::Private, kShadowName);
                    if (interfaceListsAllGlobals) entryPoint->AddOperand({SPV_OPERAND_TYPE_ID, {shadowId}});
                }

                for (Instruction* chain : memberChains) {
                    if (chain->NumInOperands() == 2) {
                        // The chain IS the whole array: every use now addresses the shadow.
                        ctx->ReplaceAllUsesWith(chain->result_id(), shadowId);
                        ctx->KillInst(chain);
                        continue;
                    }
                    Instruction::OperandList operands;
                    operands.push_back({SPV_OPERAND_TYPE_ID, {shadowId}});
                    for (uint32_t i = 2; i < chain->NumInOperands(); ++i) {
                        operands.push_back(chain->GetInOperand(i));
                    }
                    chain->SetInOperands(Move(operands));
                    chain->SetResultType(
                        typeMgr->FindPointerToType(PointeeOf(ctx, chain->type_id()), spv::StorageClass::Private));
                    defUse->AnalyzeInstUse(chain);
                    RetypeChainUsers(ctx, chain);
                }
                for (Instruction* var : standaloneVars) {
                    const uint32_t varId = var->result_id();
                    if (native) {
                        // The variable stays the built-in the flush writes; only the shader's
                        // own accesses move to the shadow.
                        ctx->ReplaceAllUsesWithPredicate(varId, shadowId, [](Instruction* user) {
                            return user->opcode() != spv::Op::OpDecorate && user->opcode() != spv::Op::OpName &&
                                   user->opcode() != spv::Op::OpEntryPoint;
                        });
                        continue;
                    }
                    ctx->KillNamesAndDecorates(varId);
                    RemoveFromInterface(entryPoint, varId);
                    ctx->ReplaceAllUsesWith(varId, shadowId);
                    ctx->KillInst(var);
                }
                if (shadowId != 0) RetypeChainUsers(ctx, defUse->GetDef(shadowId));
                if (sites.capability != nullptr && !native) ctx->KillInst(sites.capability);

                analysis::Float floatTy(32);
                const uint32_t floatTypeId = typeMgr->GetTypeInstruction(&floatTy);
                const analysis::Type* floatReg = typeMgr->GetRegisteredType(&floatTy);
                analysis::Vector vec4Ty(floatReg, 4);
                const uint32_t vec4TypeId = typeMgr->GetTypeInstruction(&vec4Ty);
                analysis::Bool boolTy;
                const uint32_t boolTypeId = typeMgr->GetTypeInstruction(&boolTy);
                const uint32_t oneId = constMgr->GetFloatConstId(1.0f);
                const uint32_t zeroId = constMgr->GetFloatConstId(0.0f);
                const uint32_t ptrPrivateFloat = typeMgr->FindPointerToType(floatTypeId, spv::StorageClass::Private);

                // ---- producer: write the carriers ----
                if (producedCount > 0) {
                    const Uint32 carrierCount = native ? 0 : (producedCount + 3) / 4;
                    uint32_t carriers[kMaxCarriers] = {0, 0};
                    const uint32_t ptrOutputFloat = typeMgr->FindPointerToType(floatTypeId, spv::StorageClass::Output);
                    const uint32_t nativeMemberConst =
                        nativeBlockVar != nullptr ? constMgr->GetSIntConstId(static_cast<int32_t>(nativeBlockMember)) : 0;
                    for (Uint32 k = 0; k < carrierCount; ++k) {
                        carriers[k] = AddGlobalVariable(ctx, vec4TypeId, spv::StorageClass::Output, CarrierName(k));
                        AddLocationDecoration(ctx, carriers[k], m_options.location + k);
                        entryPoint->AddOperand({SPV_OPERAND_TYPE_ID, {carriers[k]}});
                    }
                    uint32_t ptrUniformEnables = 0;
                    uint32_t enablesMemberConst = 0;
                    if (enablesVarId != 0) {
                        ptrUniformEnables = typeMgr->FindPointerToType(enablesTypeId, spv::StorageClass::Uniform);
                        enablesMemberConst = constMgr->GetSIntConstId(static_cast<int32_t>(enablesMemberIndex));
                        if (interfaceListsAllGlobals) {
                            bool listed = false;
                            for (uint32_t i = 3; i < entryPoint->NumInOperands(); ++i) {
                                if (entryPoint->GetSingleWordInOperand(i) == enablesVarId) listed = true;
                            }
                            if (!listed) entryPoint->AddOperand({SPV_OPERAND_TYPE_ID, {enablesVarId}});
                        }
                    }

                    std::vector<Instruction*> flushSites;
                    if (isGeometry) {
                        for (auto& function : *ctx->module()) {
                            function.ForEachInst([&](Instruction* inst) {
                                if (inst->opcode() == spv::Op::OpEmitVertex ||
                                    inst->opcode() == spv::Op::OpEmitStreamVertex) {
                                    flushSites.push_back(inst);
                                }
                            });
                        }
                    } else if (Function* entry = EntryFunction(ctx, entryPoint)) {
                        entry->ForEachInst([&](Instruction* inst) {
                            if (inst->opcode() == spv::Op::OpReturn || inst->opcode() == spv::Op::OpReturnValue) {
                                flushSites.push_back(inst);
                            }
                        });
                    }
                    for (Instruction* site : flushSites) {
                        InstructionBuilder b(ctx, site, kBuilderAnalyses);
                        uint32_t maskId = 0;
                        if (enablesVarId != 0) {
                            Instruction* ptr = b.AddAccessChain(ptrUniformEnables, enablesVarId, {enablesMemberConst});
                            maskId = b.AddLoad(enablesTypeId, ptr->result_id())->result_id();
                        }
                        std::vector<uint32_t> values;
                        for (Uint32 i = 0; i < producedCount; ++i) {
                            Instruction* ptr = b.AddAccessChain(ptrPrivateFloat, shadowId, {constMgr->GetUIntConstId(i)});
                            uint32_t value = b.AddLoad(floatTypeId, ptr->result_id())->result_id();
                            if (maskId != 0) {
                                const uint32_t bit =
                                    b.AddBinaryOp(enablesTypeId, spv::Op::OpBitwiseAnd, maskId,
                                                  IntConstantOfType(ctx, enablesTypeId, 1u << i))
                                        ->result_id();
                                const uint32_t on = b.AddBinaryOp(boolTypeId, spv::Op::OpINotEqual, bit,
                                                                  IntConstantOfType(ctx, enablesTypeId, 0u))
                                                        ->result_id();
                                value = b.AddSelect(floatTypeId, on, value, oneId)->result_id();
                            }
                            values.push_back(value);
                        }
                        if (native) {
                            for (Uint32 i = 0; i < producedCount; ++i) {
                                const uint32_t index = constMgr->GetUIntConstId(i);
                                Instruction* ptr =
                                    nativeBlockVar != nullptr
                                        ? b.AddAccessChain(ptrOutputFloat, nativeBlockVar->result_id(),
                                                           {nativeMemberConst, index})
                                        : b.AddAccessChain(ptrOutputFloat, nativeStandaloneVar->result_id(), {index});
                                b.AddStore(ptr->result_id(), values[i]);
                            }
                        }
                        while (values.size() % 4 != 0) values.push_back(oneId);
                        for (Uint32 k = 0; k < carrierCount; ++k) {
                            const uint32_t composite =
                                b.AddCompositeConstruct(vec4TypeId, {values[k * 4], values[k * 4 + 1], values[k * 4 + 2],
                                                                     values[k * 4 + 3]})
                                    ->result_id();
                            b.AddStore(carriers[k], composite);
                        }
                    }
                    if (m_report != nullptr) {
                        m_report->distanceCount = producedCount;
                        m_report->enablesFound = enablesVarId != 0;
                    }
                }

                // ---- consumer: read the carriers, discard, serve gl_ClipDistance reads ----
                if (m_options.role == Role::Consumer && consumedCount > 0) {
                    Function* entry = EntryFunction(ctx, entryPoint);
                    if (entry != nullptr && entry->begin() != entry->end()) {
                        const Uint32 carrierCount = (consumedCount + 3) / 4;
                        uint32_t carriers[kMaxCarriers] = {0, 0};
                        for (Uint32 k = 0; k < carrierCount; ++k) {
                            carriers[k] = AddGlobalVariable(ctx, vec4TypeId, spv::StorageClass::Input, CarrierName(k));
                            AddLocationDecoration(ctx, carriers[k], m_options.location + k);
                            entryPoint->AddOperand({SPV_OPERAND_TYPE_ID, {carriers[k]}});
                        }
                        BasicBlock* head = &*entry->begin();
                        auto split = head->begin();
                        while (split != head->end() && split->opcode() == spv::Op::OpVariable) ++split;
                        BasicBlock* rest = head->SplitBasicBlock(ctx, ctx->TakeNextId(), split);

                        InstructionBuilder b(ctx, head, kBuilderAnalyses);
                        uint32_t loaded[kMaxCarriers] = {0, 0};
                        for (Uint32 k = 0; k < carrierCount; ++k) {
                            loaded[k] = b.AddLoad(vec4TypeId, carriers[k])->result_id();
                        }
                        uint32_t anyNegative = 0;
                        for (Uint32 i = 0; i < consumedCount; ++i) {
                            const uint32_t component =
                                b.AddCompositeExtract(floatTypeId, loaded[i / 4], {i % 4})->result_id();
                            if (shadowId != 0 && i < shadowLength) {
                                Instruction* ptr =
                                    b.AddAccessChain(ptrPrivateFloat, shadowId, {constMgr->GetUIntConstId(i)});
                                b.AddStore(ptr->result_id(), component);
                            }
                            const uint32_t negative =
                                b.AddBinaryOp(boolTypeId, spv::Op::OpFOrdLessThan, component, zeroId)->result_id();
                            anyNegative = anyNegative == 0
                                              ? negative
                                              : b.AddBinaryOp(boolTypeId, spv::Op::OpLogicalOr, anyNegative, negative)
                                                    ->result_id();
                        }
                        const uint32_t killLabel = ctx->TakeNextId();
                        b.AddConditionalBranch(anyNegative, killLabel, rest->id(), rest->id());

                        auto killBlock = spvtools::MakeUnique<BasicBlock>(spvtools::MakeUnique<Instruction>(
                            ctx, spv::Op::OpLabel, 0, killLabel, std::initializer_list<Operand>{}));
                        killBlock->AddInstruction(spvtools::MakeUnique<Instruction>(
                            ctx, spv::Op::OpKill, 0, 0, std::initializer_list<Operand>{}));
                        entry->InsertBasicBlockAfter(std::move(killBlock), head);
                    }
                }

                if (m_report != nullptr) m_report->rewrote = true;
                ctx->InvalidateAnalysesExceptFor(IRContext::kAnalysisNone);
                return Status::SuccessWithChange;
            }

            spvtools::Optimizer::PassToken EmulateClipDistancePass::CreateEmulateClipDistancePass(
                EmulateClipDistanceOptions options, EmulateClipDistanceReport* report) {
                return spvtools::Optimizer::PassToken(MakeUnique<EmulateClipDistancePass>(Move(options), report));
            }
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
