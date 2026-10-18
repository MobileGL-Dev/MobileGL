// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/RetainGeometryOutputsPass.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "RetainGeometryOutputsPass.h"

#include "spirv.hpp"
#include "source/opt/basic_block.h"
#include "source/opt/cfg.h"
#include "source/opt/constants.h"
#include "source/opt/decoration_manager.h"
#include "source/opt/def_use_manager.h"
#include "source/opt/function.h"
#include "source/opt/instruction.h"
#include "source/opt/ir_context.h"
#include "source/opt/module.h"
#include "source/opt/type_manager.h"
#include "source/util/make_unique.h"
#include "source/util/string_utils.h"

#include <memory>
#include <unordered_map>
#include <unordered_set>
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
                using OperandList = std::vector<Operand>;
                // One bit per slot: whether the slot was fully written since the last emit.
                using WrittenSet = std::vector<Bool>;

                Operand Id(uint32_t id) {
                    return {SPV_OPERAND_TYPE_ID, {id}};
                }

                std::unique_ptr<Instruction> Make(IRContext* context, spv::Op op, uint32_t typeId, uint32_t resultId,
                                                  OperandList operands) {
                    return spvtools::MakeUnique<Instruction>(context, op, typeId, resultId, operands);
                }

                Bool IsEmit(spv::Op op) {
                    return op == spv::Op::OpEmitVertex || op == spv::Op::OpEmitStreamVertex;
                }

                Bool IsAccessChain(spv::Op op) {
                    return op == spv::Op::OpAccessChain || op == spv::Op::OpInBoundsAccessChain;
                }

                // Users that name a variable without touching its memory.
                Bool IsDeclarationUser(spv::Op op) {
                    return op == spv::Op::OpName || op == spv::Op::OpDecorate || op == spv::Op::OpDecorateId ||
                           op == spv::Op::OpDecorateString || op == spv::Op::OpEntryPoint ||
                           op == spv::Op::OpGroupDecorate;
                }

                // The value of an OpConstant integer, or nullopt for anything else.
                Optional<uint32_t> ConstantValue(IRContext* context, uint32_t id) {
                    const Instruction* def = context->get_def_use_mgr()->GetDef(id);
                    if (def == nullptr || def->opcode() != spv::Op::OpConstant || def->NumInOperands() < 1)
                        return std::nullopt;
                    return def->GetSingleWordInOperand(0);
                }

                Bool IsBlockStruct(IRContext* context, uint32_t typeId) {
                    const Instruction* type = context->get_def_use_mgr()->GetDef(typeId);
                    return type != nullptr && type->opcode() == spv::Op::OpTypeStruct &&
                           context->get_decoration_mgr()->HasDecoration(typeId, uint32_t(spv::Decoration::Block));
                }

                // Whether a Block-decorated struct sits anywhere inside this type: such a type
                // cannot be copied into a Private shadow.
                Bool ContainsBlock(IRContext* context, uint32_t typeId) {
                    const Instruction* type = context->get_def_use_mgr()->GetDef(typeId);
                    if (type == nullptr) return false;
                    switch (type->opcode()) {
                    case spv::Op::OpTypeArray:
                    case spv::Op::OpTypeRuntimeArray:
                        return ContainsBlock(context, type->GetSingleWordInOperand(0));
                    case spv::Op::OpTypeStruct:
                        if (IsBlockStruct(context, typeId)) return true;
                        for (uint32_t i = 0; i < type->NumInOperands(); ++i) {
                            if (ContainsBlock(context, type->GetSingleWordInOperand(i))) return true;
                        }
                        return false;
                    default:
                        return false;
                    }
                }

                // Whether every use of this pointer, followed through access chains, is one the
                // redirection can retarget (loads, stores, copies, further access chains), whether
                // any of them writes, and every pointer derived from it. A pointer handed to a call
                // or used as a value is not redirectable: the callee's parameter type names the
                // Output storage class.
                struct PointerUse {
                    Bool redirectable = true;
                    Bool stored = false;
                    std::vector<uint32_t> pointers;
                };
                void ScanPointerUses(IRContext* context, Instruction* pointer, PointerUse& use) {
                    use.pointers.push_back(pointer->result_id());
                    context->get_def_use_mgr()->ForEachUse(pointer, [&](Instruction* user, uint32_t operandIndex) {
                        const spv::Op op = user->opcode();
                        if (IsDeclarationUser(op)) return;
                        switch (op) {
                        case spv::Op::OpLoad:
                            break;
                        case spv::Op::OpStore:
                            if (operandIndex == 0) {
                                use.stored = true;
                            } else {
                                use.redirectable = false;
                            }
                            break;
                        case spv::Op::OpCopyMemory:
                        case spv::Op::OpCopyMemorySized:
                            if (operandIndex == 0) use.stored = true;
                            break;
                        case spv::Op::OpAccessChain:
                        case spv::Op::OpInBoundsAccessChain:
                            if (operandIndex == 2) {
                                ScanPointerUses(context, user, use);
                            } else {
                                use.redirectable = false;
                            }
                            break;
                        default:
                            use.redirectable = false;
                            break;
                        }
                    });
                }

                // One output value that may be retained: a whole output variable, or one member
                // of an output block.
                struct Slot {
                    uint32_t variableId = 0;
                    Int64 member = -1;
                    uint32_t valueTypeId = 0;
                    uint32_t blockTypeId = 0;
                    // Filled for the slots that are retained.
                    uint32_t shadowId = 0;
                    uint32_t privatePointerTypeId = 0;
                    uint32_t outputPointerTypeId = 0;
                    uint32_t memberIndexId = 0;
                };

                String NameOf(IRContext* context, uint32_t id) {
                    for (const Instruction& debugInst : context->debugs2()) {
                        if (debugInst.opcode() == spv::Op::OpName && debugInst.GetSingleWordInOperand(0) == id)
                            return debugInst.GetInOperand(1).AsString();
                    }
                    return {};
                }

                String MemberNameOf(IRContext* context, uint32_t structTypeId, uint32_t member) {
                    for (const Instruction& debugInst : context->debugs2()) {
                        if (debugInst.opcode() == spv::Op::OpMemberName &&
                            debugInst.GetSingleWordInOperand(0) == structTypeId &&
                            debugInst.GetSingleWordInOperand(1) == member)
                            return debugInst.GetInOperand(2).AsString();
                    }
                    return {};
                }

                // The shadow's debug name. A builtin's `gl_` prefix is dropped so the generated
                // source never spells a reserved name, nor one a text pass searches for.
                String ShadowName(String outputName) {
                    if (outputName.empty()) return {};
                    if (outputName.rfind("gl_", 0) == 0) outputName.erase(0, 3);
                    while (!outputName.empty() && outputName.front() == '_')
                        outputName.erase(0, 1);
                    return outputName.empty() ? String{} : "mg_retained_" + outputName;
                }

                // Which slot each pointer addresses: `whole` holds the pointers a store through
                // which rewrites the whole slot, `any` every pointer into a slot.
                struct SlotPointers {
                    std::unordered_map<uint32_t, SizeT> whole;
                    std::unordered_map<uint32_t, SizeT> any;
                };

                // Runs a must-be-written dataflow over one function and marks every slot whose
                // value from before an earlier emit can be observed: at an emit (not fully written
                // since the previous one) or by a read (same condition). The entry function starts
                // with every slot counted as written, as nothing has been emitted yet; any other
                // function starts with none, since its caller may have emitted. A call that can
                // emit acts like an emit; writes inside callees are not followed.
                void MarkStaleSlots(IRContext* context, Function& function, Bool isEntryFunction,
                                    const SlotPointers& slotPointers,
                                    const std::unordered_set<uint32_t>& emittingFunctions, SizeT slotCount,
                                    std::vector<Bool>& stale) {
                    const WrittenSet all(slotCount, true);
                    const WrittenSet none(slotCount, false);
                    auto transfer = [&](BasicBlock& block, WrittenSet& written, Bool check) {
                        auto read = [&](uint32_t pointer) {
                            const auto slot = slotPointers.any.find(pointer);
                            if (check && slot != slotPointers.any.end() && !written[slot->second])
                                stale[slot->second] = true;
                        };
                        for (Instruction& inst : block) {
                            const spv::Op op = inst.opcode();
                            if (IsEmit(op)) {
                                if (check) {
                                    for (SizeT slot = 0; slot < slotCount; ++slot) {
                                        if (!written[slot]) stale[slot] = true;
                                    }
                                }
                                written = none;
                            } else if (op == spv::Op::OpFunctionCall) {
                                if (emittingFunctions.count(inst.GetSingleWordInOperand(0)) != 0) written = none;
                            } else if (op == spv::Op::OpLoad) {
                                read(inst.GetSingleWordInOperand(0));
                            } else if (op == spv::Op::OpStore || op == spv::Op::OpCopyMemory ||
                                       op == spv::Op::OpCopyMemorySized) {
                                if (op != spv::Op::OpStore) read(inst.GetSingleWordInOperand(1));
                                const auto target = slotPointers.whole.find(inst.GetSingleWordInOperand(0));
                                if (target != slotPointers.whole.end()) written[target->second] = true;
                            }
                        }
                    };

                    spvtools::opt::CFG* cfg = context->cfg();
                    const uint32_t entryLabel = function.begin()->id();
                    std::unordered_map<uint32_t, WrittenSet> out;
                    for (BasicBlock& block : function)
                        out[block.id()] = all;
                    auto entryState = [&](BasicBlock& block) {
                        if (block.id() == entryLabel) return isEntryFunction ? all : none;
                        WrittenSet written = all;
                        for (uint32_t predecessor : cfg->preds(block.id())) {
                            const auto found = out.find(predecessor);
                            if (found == out.end()) continue;
                            for (SizeT slot = 0; slot < slotCount; ++slot)
                                written[slot] = written[slot] && found->second[slot];
                        }
                        return written;
                    };
                    for (Bool changed = true; changed;) {
                        changed = false;
                        for (BasicBlock& block : function) {
                            WrittenSet written = entryState(block);
                            transfer(block, written, false);
                            if (written != out[block.id()]) {
                                out[block.id()] = std::move(written);
                                changed = true;
                            }
                        }
                    }
                    for (BasicBlock& block : function) {
                        WrittenSet written = entryState(block);
                        transfer(block, written, true);
                    }
                }
            } // namespace

            spvtools::opt::Pass::Status RetainGeometryOutputsPass::Process() {
                IRContext* irContext = context();
                auto* defUse = irContext->get_def_use_mgr();

                // One geometry entry point.
                Instruction* entryPoint = nullptr;
                for (Instruction& candidate : irContext->module()->entry_points()) {
                    if (entryPoint != nullptr) return Status::SuccessWithoutChange;
                    entryPoint = &candidate;
                }
                if (entryPoint == nullptr || static_cast<spv::ExecutionModel>(entryPoint->GetSingleWordInOperand(0)) !=
                                                 spv::ExecutionModel::Geometry) {
                    return Status::SuccessWithoutChange;
                }
                const uint32_t entryFunctionId = entryPoint->GetSingleWordInOperand(1);

                // Every emit, and every function a call to which can emit.
                std::vector<Instruction*> emits;
                std::unordered_set<uint32_t> emittingFunctions;
                for (Function& function : *irContext->module()) {
                    function.ForEachInst([&](Instruction* inst) {
                        if (!IsEmit(inst->opcode())) return;
                        emits.push_back(inst);
                        emittingFunctions.insert(function.result_id());
                    });
                }
                if (emits.empty()) return Status::SuccessWithoutChange;
                for (Bool grew = true; grew;) {
                    grew = false;
                    for (Function& function : *irContext->module()) {
                        if (emittingFunctions.count(function.result_id()) != 0) continue;
                        Bool callsEmitter = false;
                        function.ForEachInst([&](Instruction* inst) {
                            if (inst->opcode() == spv::Op::OpFunctionCall &&
                                emittingFunctions.count(inst->GetSingleWordInOperand(0)) != 0)
                                callsEmitter = true;
                        });
                        if (callsEmitter) {
                            emittingFunctions.insert(function.result_id());
                            grew = true;
                        }
                    }
                }

                // The candidate slots: every output, or output block member, the shader writes and
                // whose every use can be redirected.
                std::vector<Slot> slots;
                SlotPointers slotPointers;
                for (Instruction& global : irContext->types_values()) {
                    if (global.opcode() != spv::Op::OpVariable ||
                        static_cast<spv::StorageClass>(global.GetSingleWordInOperand(0)) != spv::StorageClass::Output) {
                        continue;
                    }
                    const uint32_t valueTypeId = defUse->GetDef(global.type_id())->GetSingleWordInOperand(1);
                    if (!IsBlockStruct(irContext, valueTypeId)) {
                        if (ContainsBlock(irContext, valueTypeId)) continue;
                        PointerUse use;
                        ScanPointerUses(irContext, &global, use);
                        if (!use.stored || !use.redirectable) continue;
                        slotPointers.whole[global.result_id()] = slots.size();
                        for (uint32_t pointer : use.pointers)
                            slotPointers.any[pointer] = slots.size();
                        slots.push_back({global.result_id(), -1, valueTypeId});
                        continue;
                    }

                    // A block, member by member. A use of the block as a whole (never emitted by
                    // the front end) leaves the whole block alone.
                    const Instruction* blockType = defUse->GetDef(valueTypeId);
                    const uint32_t memberCount = blockType->NumInOperands();
                    std::vector<PointerUse> memberUses(memberCount);
                    std::vector<std::vector<uint32_t>> memberWholePointers(memberCount);
                    Bool wholeBlockUse = false;
                    defUse->ForEachUse(&global, [&](Instruction* user, uint32_t operandIndex) {
                        if (IsDeclarationUser(user->opcode())) return;
                        if (!IsAccessChain(user->opcode()) || operandIndex != 2 || user->NumInOperands() < 2) {
                            wholeBlockUse = true;
                            return;
                        }
                        const auto member = ConstantValue(irContext, user->GetSingleWordInOperand(1));
                        if (!member || *member >= memberCount) {
                            wholeBlockUse = true;
                            return;
                        }
                        ScanPointerUses(irContext, user, memberUses[*member]);
                        if (user->NumInOperands() == 2) memberWholePointers[*member].push_back(user->result_id());
                    });
                    if (wholeBlockUse) continue;
                    for (uint32_t member = 0; member < memberCount; ++member) {
                        if (!memberUses[member].stored || !memberUses[member].redirectable) continue;
                        for (uint32_t pointer : memberWholePointers[member])
                            slotPointers.whole[pointer] = slots.size();
                        for (uint32_t pointer : memberUses[member].pointers)
                            slotPointers.any[pointer] = slots.size();
                        slots.push_back({global.result_id(), Int64(member), blockType->GetSingleWordInOperand(member),
                                         valueTypeId});
                    }
                }
                if (slots.empty()) return Status::SuccessWithoutChange;

                // Which slots can be observed holding a value from before an earlier emit.
                std::vector<Bool> stale(slots.size(), false);
                for (Function& function : *irContext->module()) {
                    MarkStaleSlots(irContext, function, function.result_id() == entryFunctionId, slotPointers,
                                   emittingFunctions, slots.size(), stale);
                }
                std::vector<Slot> retained;
                for (SizeT slot = 0; slot < slots.size(); ++slot) {
                    if (stale[slot]) retained.push_back(slots[slot]);
                }
                if (retained.empty()) return Status::SuccessWithoutChange;

                // ---- Types and constants, then the shadows, before any instruction changes ----
                auto* typeMgr = irContext->get_type_mgr();
                auto* constMgr = irContext->get_constant_mgr();
                for (Slot& slot : retained) {
                    slot.privatePointerTypeId =
                        typeMgr->FindPointerToType(slot.valueTypeId, spv::StorageClass::Private);
                    if (slot.member >= 0) {
                        slot.outputPointerTypeId =
                            typeMgr->FindPointerToType(slot.valueTypeId, spv::StorageClass::Output);
                        slot.memberIndexId = constMgr->GetUIntConstId(uint32_t(slot.member));
                    }
                }

                // Every access chain derived from a redirected pointer changes storage class with
                // it; collected (with the Private pointer type it needs) while def-use is intact.
                std::vector<std::pair<Instruction*, uint32_t>> retypes;
                std::unordered_set<Instruction*> retypeSeen;
                auto collectRetypes = [&](auto&& self, Instruction* pointer) -> void {
                    defUse->ForEachUser(pointer, [&](Instruction* user) {
                        if (!IsAccessChain(user->opcode()) || user->GetSingleWordInOperand(0) != pointer->result_id())
                            return;
                        if (!retypeSeen.insert(user).second) return;
                        const uint32_t pointeeTypeId = defUse->GetDef(user->type_id())->GetSingleWordInOperand(1);
                        retypes.emplace_back(user,
                                             typeMgr->FindPointerToType(pointeeTypeId, spv::StorageClass::Private));
                        self(self, user);
                    });
                };

                // Operand rewrites: (instruction, operand index counting result type and id, new id).
                struct Rewrite {
                    Instruction* inst;
                    uint32_t operand;
                    uint32_t newId;
                };
                std::vector<Rewrite> rewrites;
                // Single-index chains onto a retained block member: the shadow replaces them.
                std::vector<Instruction*> aliasChains;
                // Deeper chains onto a retained block member: rebased onto the shadow.
                std::vector<std::pair<Instruction*, Slot*>> rebasedChains;

                for (Slot& slot : retained) {
                    slot.shadowId = irContext->TakeNextId();
                    Instruction* variable = defUse->GetDef(slot.variableId);
                    if (slot.member < 0) {
                        defUse->ForEachUse(variable, [&](Instruction* user, uint32_t operandIndex) {
                            if (!IsDeclarationUser(user->opcode()))
                                rewrites.push_back({user, operandIndex, slot.shadowId});
                        });
                        collectRetypes(collectRetypes, variable);
                        continue;
                    }
                    defUse->ForEachUser(variable, [&](Instruction* user) {
                        if (!IsAccessChain(user->opcode()) || user->NumInOperands() < 2) return;
                        const auto member = ConstantValue(irContext, user->GetSingleWordInOperand(1));
                        if (!member || Int64(*member) != slot.member) return;
                        if (user->NumInOperands() == 2) {
                            aliasChains.push_back(user);
                            defUse->ForEachUse(user, [&](Instruction* aliasUser, uint32_t operandIndex) {
                                if (!IsDeclarationUser(aliasUser->opcode()))
                                    rewrites.push_back({aliasUser, operandIndex, slot.shadowId});
                            });
                        } else {
                            rebasedChains.emplace_back(user, &slot);
                            const uint32_t pointeeTypeId = defUse->GetDef(user->type_id())->GetSingleWordInOperand(1);
                            retypes.emplace_back(user,
                                                 typeMgr->FindPointerToType(pointeeTypeId, spv::StorageClass::Private));
                            retypeSeen.insert(user);
                        }
                        collectRetypes(collectRetypes, user);
                    });
                }

                for (Slot& slot : retained) {
                    irContext->AddGlobalValue(
                        Make(irContext, spv::Op::OpVariable, slot.privatePointerTypeId, slot.shadowId,
                             {{SPV_OPERAND_TYPE_STORAGE_CLASS, {uint32_t(spv::StorageClass::Private)}}}));
                    const String outputName = slot.member < 0
                                                  ? NameOf(irContext, slot.variableId)
                                                  : MemberNameOf(irContext, slot.blockTypeId, uint32_t(slot.member));
                    const String shadowName = ShadowName(outputName);
                    if (!shadowName.empty()) {
                        irContext->AddDebug2Inst(
                            Make(irContext, spv::Op::OpName, 0, 0,
                                 {Id(slot.shadowId),
                                  {SPV_OPERAND_TYPE_LITERAL_STRING, spvtools::utils::MakeVector(shadowName.c_str())}}));
                    }
                    // From SPIR-V 1.4 the interface lists every global the entry point uses.
                    if (irContext->module()->version() >= 0x00010400u) entryPoint->AddOperand(Id(slot.shadowId));
                }

                // ---- Hand-built from here on; analyses are rebuilt once at the end ----------
                irContext->InvalidateAnalysesExceptFor(IRContext::kAnalysisNone);

                for (const Rewrite& rewrite : rewrites)
                    rewrite.inst->SetOperand(rewrite.operand, {rewrite.newId});
                for (auto& [chain, slot] : rebasedChains) {
                    OperandList operands;
                    operands.push_back(Id(slot->shadowId));
                    for (uint32_t i = 2; i < chain->NumInOperands(); ++i)
                        operands.push_back(chain->GetInOperand(i));
                    chain->SetInOperands(std::move(operands));
                }
                for (auto& [chain, typeId] : retypes)
                    chain->SetResultType(typeId);
                for (Instruction* chain : aliasChains)
                    irContext->KillInst(chain);

                // The copy into the real outputs, immediately before every emit.
                for (Instruction* emit : emits) {
                    for (const Slot& slot : retained) {
                        const uint32_t value = irContext->TakeNextId();
                        emit->InsertBefore(
                            Make(irContext, spv::Op::OpLoad, slot.valueTypeId, value, {Id(slot.shadowId)}));
                        uint32_t target = slot.variableId;
                        if (slot.member >= 0) {
                            target = irContext->TakeNextId();
                            emit->InsertBefore(Make(irContext, spv::Op::OpAccessChain, slot.outputPointerTypeId, target,
                                                    {Id(slot.variableId), Id(slot.memberIndexId)}));
                        }
                        emit->InsertBefore(Make(irContext, spv::Op::OpStore, 0, 0, {Id(target), Id(value)}));
                    }
                }

                irContext->InvalidateAnalysesExceptFor(IRContext::kAnalysisNone);
                return Status::SuccessWithChange;
            }

            spvtools::Optimizer::PassToken RetainGeometryOutputsPass::CreateRetainGeometryOutputsPass() {
                return spvtools::Optimizer::PassToken(spvtools::MakeUnique<RetainGeometryOutputsPass>());
            }
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
