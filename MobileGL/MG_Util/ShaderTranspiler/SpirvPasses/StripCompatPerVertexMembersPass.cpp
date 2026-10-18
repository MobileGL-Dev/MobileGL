// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/StripCompatPerVertexMembersPass.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "StripCompatPerVertexMembersPass.h"

#include "spirv.hpp"
#include "source/opt/constants.h"
#include "source/opt/def_use_manager.h"
#include "source/opt/instruction.h"
#include "source/opt/ir_context.h"
#include "source/opt/module.h"
#include "source/util/make_unique.h"

#include <map>
#include <set>
#include <vector>

namespace MobileGL {
    namespace MG_Util {
        namespace ShaderTranspiler {
            namespace {
                using spvtools::opt::Instruction;
                using spvtools::opt::IRContext;
                using spvtools::opt::Operand;

                // A struct that is a Block, has at least one BuiltIn member and at least one
                // member without: the shape glslang gives a compatibility-profile gl_PerVertex.
                struct MixedBlock {
                    Instruction* type = nullptr;
                    std::set<uint32_t> builtInMembers;
                    // Every member index some access chain reaches, across all its variables.
                    std::set<uint32_t> accessedMembers;
                    // (access chain, in-operand) pairs that name a member, rewritten on a strip.
                    std::vector<std::pair<Instruction*, uint32_t>> memberOperands;
                    bool rejected = false;
                };

                bool IsAccessChain(spv::Op op) {
                    return op == spv::Op::OpAccessChain || op == spv::Op::OpInBoundsAccessChain;
                }

                bool IsNameOrDecoration(const Instruction& inst) {
                    return inst.IsDecoration() || inst.opcode() == spv::Op::OpName ||
                           inst.opcode() == spv::Op::OpMemberName;
                }

                std::map<uint32_t, MixedBlock> CollectMixedBlocks(IRContext* ctx) {
                    std::set<uint32_t> blocks;
                    std::map<uint32_t, std::set<uint32_t>> builtIns;
                    for (Instruction& ann : ctx->annotations()) {
                        if (ann.opcode() == spv::Op::OpDecorate &&
                            static_cast<spv::Decoration>(ann.GetSingleWordInOperand(1)) == spv::Decoration::Block) {
                            blocks.insert(ann.GetSingleWordInOperand(0));
                        } else if (ann.opcode() == spv::Op::OpMemberDecorate &&
                                   static_cast<spv::Decoration>(ann.GetSingleWordInOperand(2)) ==
                                       spv::Decoration::BuiltIn) {
                            builtIns[ann.GetSingleWordInOperand(0)].insert(ann.GetSingleWordInOperand(1));
                        }
                    }
                    std::map<uint32_t, MixedBlock> mixed;
                    for (auto& [structId, members] : builtIns) {
                        if (blocks.count(structId) == 0) continue;
                        Instruction* type = ctx->get_def_use_mgr()->GetDef(structId);
                        if (type == nullptr || type->opcode() != spv::Op::OpTypeStruct) continue;
                        if (members.size() >= type->NumInOperands()) continue;
                        mixed[structId] = MixedBlock{type, members};
                    }
                    return mixed;
                }

                // The struct's only uses may be names, decorations, Input/Output pointer types
                // and arrays of it, so no value of the whole block exists anywhere. A decoration
                // group could hide a BuiltIn from CollectMixedBlocks, so it rejects too.
                bool OnlyInterfaceTypesUse(IRContext* ctx, uint32_t typeId, bool allowArray) {
                    return ctx->get_def_use_mgr()->WhileEachUser(typeId, [&](Instruction* user) {
                        if (user->opcode() == spv::Op::OpGroupMemberDecorate) return false;
                        if (IsNameOrDecoration(*user)) return true;
                        if (user->opcode() == spv::Op::OpTypeArray && allowArray) {
                            return OnlyInterfaceTypesUse(ctx, user->result_id(), false);
                        }
                        if (user->opcode() != spv::Op::OpTypePointer) return false;
                        const auto storage = static_cast<spv::StorageClass>(user->GetSingleWordInOperand(0));
                        return storage == spv::StorageClass::Input || storage == spv::StorageClass::Output;
                    });
                }

                // Follows every use of a pointer to the block (`arrayed`: to an array of it).
                // Only access chains are followed; anything else that takes the pointer rejects.
                void WalkPointerUses(IRContext* ctx, Instruction* pointer, bool arrayed, MixedBlock& block) {
                    ctx->get_def_use_mgr()->ForEachUse(pointer, [&](Instruction* user, uint32_t operandIndex) {
                        if (block.rejected) return;
                        if (IsNameOrDecoration(*user) || user->opcode() == spv::Op::OpEntryPoint) return;
                        // Operand 2 of an access chain is its base (result type and id come first).
                        if (!IsAccessChain(user->opcode()) || operandIndex != 2) {
                            block.rejected = true;
                            return;
                        }
                        const uint32_t memberOperand = arrayed ? 2 : 1;
                        if (user->NumInOperands() <= memberOperand) {
                            // Stops at the block or the array: the result is another such pointer.
                            const bool stillArrayed = arrayed && user->NumInOperands() == 1;
                            WalkPointerUses(ctx, user, stillArrayed, block);
                            return;
                        }
                        const uint32_t indexId = user->GetSingleWordInOperand(memberOperand);
                        const auto* index = ctx->get_constant_mgr()->FindDeclaredConstant(indexId);
                        if (index == nullptr || index->AsIntConstant() == nullptr) {
                            block.rejected = true;
                            return;
                        }
                        block.accessedMembers.insert(index->GetU32());
                        block.memberOperands.emplace_back(user, memberOperand);
                    });
                }

                // The struct the Input/Output variable points at, and whether it is arrayed.
                uint32_t VariableBlockType(IRContext* ctx, const Instruction& var, bool& arrayed) {
                    Instruction* pointer = ctx->get_def_use_mgr()->GetDef(var.type_id());
                    if (pointer == nullptr || pointer->opcode() != spv::Op::OpTypePointer) return 0;
                    Instruction* pointee = ctx->get_def_use_mgr()->GetDef(pointer->GetSingleWordInOperand(1));
                    arrayed = pointee != nullptr && pointee->opcode() == spv::Op::OpTypeArray;
                    if (arrayed) pointee = ctx->get_def_use_mgr()->GetDef(pointee->GetSingleWordInOperand(0));
                    return pointee != nullptr && pointee->opcode() == spv::Op::OpTypeStruct ? pointee->result_id() : 0;
                }

                // Kills an index constant or the array type of a dropped member, and the array
                // length behind it, once nothing but names and decorations use them.
                void KillIfUnused(IRContext* ctx, uint32_t id) {
                    Instruction* def = ctx->get_def_use_mgr()->GetDef(id);
                    if (def == nullptr) return;
                    bool used = false;
                    ctx->get_def_use_mgr()->ForEachUser(def, [&](Instruction* user) {
                        if (!IsNameOrDecoration(*user)) used = true;
                    });
                    if (used) return;
                    const bool isArray = def->opcode() == spv::Op::OpTypeArray;
                    const uint32_t lengthId = isArray ? def->GetSingleWordInOperand(1) : 0;
                    if (!isArray && def->opcode() != spv::Op::OpConstant) return;
                    ctx->KillInst(def);
                    if (lengthId != 0) KillIfUnused(ctx, lengthId);
                }

                // Returns whether the module changed. The block stays as it was when a renumbered
                // index constant cannot be declared.
                bool StripBlock(IRContext* ctx, MixedBlock& block) {
                    const uint32_t structId = block.type->result_id();
                    std::map<uint32_t, uint32_t> newIndexOf;
                    for (const uint32_t member : block.builtInMembers) {
                        const uint32_t next = static_cast<uint32_t>(newIndexOf.size());
                        newIndexOf[member] = next;
                    }

                    // Constants first, while the type manager still sees the original struct.
                    auto* constants = ctx->get_constant_mgr();
                    std::vector<uint32_t> newIndexIds;
                    for (auto& [chain, operand] : block.memberOperands) {
                        const auto* oldIndex = constants->FindDeclaredConstant(chain->GetSingleWordInOperand(operand));
                        const uint32_t newIndex = newIndexOf.at(oldIndex->GetU32());
                        if (newIndex == oldIndex->GetU32()) {
                            newIndexIds.push_back(0);
                            continue;
                        }
                        Instruction* constant =
                            constants->GetDefiningInstruction(constants->GetConstant(oldIndex->type(), {newIndex}));
                        if (constant == nullptr) return !newIndexIds.empty();
                        newIndexIds.push_back(constant->result_id());
                    }
                    std::set<uint32_t> oldIndexIds;
                    for (SizeT i = 0; i < block.memberOperands.size(); ++i) {
                        if (newIndexIds[i] == 0) continue;
                        auto& [chain, operand] = block.memberOperands[i];
                        oldIndexIds.insert(chain->GetSingleWordInOperand(operand));
                        chain->SetInOperand(operand, {newIndexIds[i]});
                        ctx->get_def_use_mgr()->AnalyzeInstUse(chain);
                    }
                    for (const uint32_t id : oldIndexIds) {
                        KillIfUnused(ctx, id);
                    }

                    std::vector<Instruction*> dropped;
                    const auto renumber = [&](Instruction& inst, uint32_t memberOperand) {
                        if (inst.GetSingleWordInOperand(0) != structId) return;
                        const auto kept = newIndexOf.find(inst.GetSingleWordInOperand(memberOperand));
                        if (kept == newIndexOf.end()) {
                            dropped.push_back(&inst);
                        } else {
                            inst.SetInOperand(memberOperand, {kept->second});
                        }
                    };
                    for (Instruction& ann : ctx->annotations()) {
                        if (ann.opcode() == spv::Op::OpMemberDecorate ||
                            ann.opcode() == spv::Op::OpMemberDecorateString) {
                            renumber(ann, 1);
                        }
                    }
                    for (Instruction& debug : ctx->module()->debugs2()) {
                        if (debug.opcode() == spv::Op::OpMemberName) renumber(debug, 1);
                    }
                    for (Instruction* inst : dropped) {
                        ctx->KillInst(inst);
                    }

                    std::vector<uint32_t> droppedTypes;
                    Instruction::OperandList members;
                    for (uint32_t i = 0; i < block.type->NumInOperands(); ++i) {
                        const uint32_t memberType = block.type->GetSingleWordInOperand(i);
                        if (newIndexOf.count(i) != 0) {
                            members.push_back({SPV_OPERAND_TYPE_ID, {memberType}});
                        } else {
                            droppedTypes.push_back(memberType);
                        }
                    }
                    block.type->SetInOperands(std::move(members));
                    ctx->get_def_use_mgr()->AnalyzeInstUse(block.type);
                    for (const uint32_t typeId : droppedTypes) {
                        KillIfUnused(ctx, typeId);
                    }
                    return true;
                }
            } // namespace

            spvtools::opt::Pass::Status StripCompatPerVertexMembersPass::Process() {
                IRContext* ctx = context();
                std::map<uint32_t, MixedBlock> blocks = CollectMixedBlocks(ctx);
                if (blocks.empty()) return Status::SuccessWithoutChange;

                for (auto& [structId, block] : blocks) {
                    block.rejected = !OnlyInterfaceTypesUse(ctx, structId, true);
                }
                for (Instruction& inst : ctx->module()->types_values()) {
                    if (inst.opcode() != spv::Op::OpVariable) continue;
                    bool arrayed = false;
                    const auto found = blocks.find(VariableBlockType(ctx, inst, arrayed));
                    if (found == blocks.end() || found->second.rejected) continue;
                    WalkPointerUses(ctx, &inst, arrayed, found->second);
                }

                bool changed = false;
                for (auto& [structId, block] : blocks) {
                    if (block.rejected) continue;
                    bool legacyMemberUsed = false;
                    for (const uint32_t member : block.accessedMembers) {
                        legacyMemberUsed |= block.builtInMembers.count(member) == 0;
                    }
                    if (legacyMemberUsed) continue;
                    changed |= StripBlock(ctx, block);
                }
                return changed ? Status::SuccessWithChange : Status::SuccessWithoutChange;
            }

            spvtools::Optimizer::PassToken StripCompatPerVertexMembersPass::CreateStripCompatPerVertexMembersPass() {
                return spvtools::Optimizer::PassToken(spvtools::MakeUnique<StripCompatPerVertexMembersPass>());
            }
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
