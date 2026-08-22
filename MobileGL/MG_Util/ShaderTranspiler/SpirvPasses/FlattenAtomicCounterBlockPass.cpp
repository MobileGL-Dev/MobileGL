// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/FlattenAtomicCounterBlockPass.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "FlattenAtomicCounterBlockPass.h"

#include "spirv.hpp"
#include "source/opt/basic_block.h"
#include "source/opt/build_module.h"
#include "source/opt/constants.h"
#include "source/opt/decoration_manager.h"
#include "source/opt/def_use_manager.h"
#include "source/opt/function.h"
#include "source/opt/instruction.h"
#include "source/opt/ir_builder.h"
#include "source/opt/ir_context.h"
#include "source/opt/module.h"
#include "source/opt/type_manager.h"
#include "source/util/make_unique.h"

#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace MobileGL {
    namespace MG_Util {
        namespace ShaderTranspiler {
            namespace {
                using spvtools::MakeUnique;
                using spvtools::opt::BasicBlock;
                using spvtools::opt::Function;
                using spvtools::opt::Instruction;
                using spvtools::opt::InstructionBuilder;
                using spvtools::opt::IRContext;
                using spvtools::opt::Module;
                using spvtools::opt::Operand;

                // Kept in step with MG_Util/ShaderTranspiler/Types.h's
                // MAX_ATOMIC_COUNTER_BUFFER_SIZE (16384 bytes), expressed in uint elements. A
                // block whose declared byte window is wider than GL will ever let an application
                // bind is refused rather than expanded into a huge array.
                constexpr uint32_t kMaxCounterElements = 16384u / 4u;
                // The lowered block's name always starts with this; the spelling lives in
                // Types.h as ATOMIC_COUNTER_BLOCK_PREFIX, which is what the rest of MobileGL
                // matches on. Repeated rather than included because that header pulls the whole
                // backend-parameter surface into a pass that needs one string.
                constexpr const char* kAtomicCounterBlockPrefix = "gl_AtomicCounterBlock";
                // The stride the flattened array is laid out with, and the size of one counter.
                constexpr uint32_t kCounterBytes = 4;

                struct MemberPlan {
                    // Where this member starts, in uint elements from the block's byte 0.
                    uint32_t elementOffset = 0;
                    // How many uints it occupies: 1 for a scalar counter, N for `atomic_uint c[N]`.
                    uint32_t elementCount = 1;
                    bool isArray = false;
                };

                struct BlockPlan {
                    Instruction* structType = nullptr;
                    uint32_t uintTypeId = 0;
                    std::vector<MemberPlan> members;
                    // Access chains rooted at a variable of this block, in the order found.
                    std::vector<Instruction*> chains;
                    uint32_t totalElements = 0;
                };

                bool NameStartsWithAtomicCounterBlockPrefix(IRContext* context, uint32_t id) {
                    for (const Instruction& debug : context->module()->debugs2()) {
                        if (debug.opcode() != spv::Op::OpName || debug.NumInOperands() < 2) continue;
                        if (debug.GetSingleWordInOperand(0) != id) continue;
                        const std::string name = debug.GetInOperand(1).AsString();
                        return name.compare(0, std::strlen(kAtomicCounterBlockPrefix),
                                            kAtomicCounterBlockPrefix) == 0;
                    }
                    return false;
                }

                // The literal of the first OpMemberDecorate <structId> <member> <kind>, or none.
                bool TryGetMemberDecorationLiteral(IRContext* context, uint32_t structId, uint32_t member,
                                                   spv::Decoration kind, uint32_t* literal) {
                    for (Instruction* decoration :
                         context->get_decoration_mgr()->GetDecorationsFor(structId, false)) {
                        if (decoration->opcode() != spv::Op::OpMemberDecorate ||
                            decoration->NumInOperands() < 4 ||
                            decoration->GetSingleWordInOperand(1) != member ||
                            static_cast<spv::Decoration>(decoration->GetSingleWordInOperand(2)) != kind) {
                            continue;
                        }
                        *literal = decoration->GetSingleWordInOperand(3);
                        return true;
                    }
                    return false;
                }

                bool TryGetDecorationLiteral(IRContext* context, uint32_t id, spv::Decoration kind,
                                             uint32_t* literal) {
                    for (Instruction* decoration : context->get_decoration_mgr()->GetDecorationsFor(id, false)) {
                        if (decoration->opcode() != spv::Op::OpDecorate || decoration->NumInOperands() < 3 ||
                            static_cast<spv::Decoration>(decoration->GetSingleWordInOperand(1)) != kind) {
                            continue;
                        }
                        *literal = decoration->GetSingleWordInOperand(2);
                        return true;
                    }
                    return false;
                }

                bool IsUint32Type(const Instruction* type) {
                    return type != nullptr && type->opcode() == spv::Op::OpTypeInt &&
                           type->NumInOperands() >= 2 && type->GetSingleWordInOperand(0) == 32u &&
                           type->GetSingleWordInOperand(1) == 0u;
                }

                // The member's shape as this pass needs it, or false when it is one the pass
                // cannot re-index.
                bool DescribeMember(IRContext* context, uint32_t memberTypeId, uint32_t* uintTypeId,
                                    MemberPlan* plan) {
                    auto* defUseMgr = context->get_def_use_mgr();
                    Instruction* memberType = defUseMgr->GetDef(memberTypeId);
                    if (memberType == nullptr) return false;

                    if (IsUint32Type(memberType)) {
                        plan->isArray = false;
                        plan->elementCount = 1;
                        *uintTypeId = memberTypeId;
                        return true;
                    }
                    if (memberType->opcode() != spv::Op::OpTypeArray || memberType->NumInOperands() < 2) {
                        return false;
                    }
                    const uint32_t elementTypeId = memberType->GetSingleWordInOperand(0);
                    if (!IsUint32Type(defUseMgr->GetDef(elementTypeId))) return false;
                    // The array's stride must be the tight 4 for the flattening to keep every
                    // counter on the byte it was declared at.
                    uint32_t stride = 0;
                    if (!TryGetDecorationLiteral(context, memberTypeId, spv::Decoration::ArrayStride, &stride) ||
                        stride != kCounterBytes) {
                        return false;
                    }
                    const spvtools::opt::analysis::Constant* length =
                        context->get_constant_mgr()->FindDeclaredConstant(memberType->GetSingleWordInOperand(1));
                    if (length == nullptr || length->AsIntConstant() == nullptr) return false;
                    const uint32_t count = length->AsIntConstant()->GetU32BitValue();
                    if (count == 0u) return false;
                    plan->isArray = true;
                    plan->elementCount = count;
                    *uintTypeId = elementTypeId;
                    return true;
                }

                // Whether the members' offsets already ARE the natural std430 packing, i.e.
                // whether the block transpiles as it stands and this pass must leave it alone.
                bool IsNaturallyPacked(const std::vector<MemberPlan>& members) {
                    uint32_t natural = 0;
                    for (const MemberPlan& member : members) {
                        if (member.elementOffset != natural) return false;
                        natural += member.elementCount;
                    }
                    return true;
                }

                // Plans every atomic-counter block the module declares that is NOT already
                // naturally packed and that this pass can re-index exactly. Reads the module;
                // never rewrites it, so the same walk serves both the detection probe and phase 1
                // of the rewrite.
                std::vector<BlockPlan> BuildPlans(IRContext* context) {
                    std::vector<BlockPlan> plans;
                    auto* defUseMgr = context->get_def_use_mgr();

                    std::unordered_map<uint32_t, Instruction*> candidateStructs;
                    for (Instruction& inst : context->module()->types_values()) {
                        if (inst.opcode() != spv::Op::OpTypeStruct || inst.NumInOperands() == 0) continue;
                        if (!NameStartsWithAtomicCounterBlockPrefix(context, inst.result_id())) continue;
                        candidateStructs.emplace(inst.result_id(), &inst);
                    }
                    if (candidateStructs.empty()) return plans;

                    std::unordered_map<uint32_t, uint32_t> variableToStruct;
                    for (Instruction& inst : context->module()->types_values()) {
                        if (inst.opcode() != spv::Op::OpVariable) continue;
                        Instruction* pointerType = defUseMgr->GetDef(inst.type_id());
                        if (pointerType == nullptr || pointerType->opcode() != spv::Op::OpTypePointer) continue;
                        if (candidateStructs.count(pointerType->GetSingleWordInOperand(1)) == 0) continue;
                        variableToStruct.emplace(inst.result_id(), pointerType->GetSingleWordInOperand(1));
                    }
                    if (variableToStruct.empty()) return plans;

                    // A block whose variable is used as anything but an access-chain base (loaded
                    // whole, handed to a function) cannot be re-indexed; a partially re-indexed
                    // block would address the wrong counters, so the whole block is refused.
                    std::unordered_map<uint32_t, std::vector<Instruction*>> chainsByStruct;
                    std::unordered_set<uint32_t> undoableStructs;
                    for (const auto& [variableId, structId] : variableToStruct) {
                        defUseMgr->ForEachUser(defUseMgr->GetDef(variableId), [&](Instruction* user) {
                            switch (user->opcode()) {
                            case spv::Op::OpName:
                            case spv::Op::OpDecorate:
                            case spv::Op::OpDecorateId:
                            case spv::Op::OpEntryPoint:
                                return;
                            case spv::Op::OpAccessChain:
                            case spv::Op::OpInBoundsAccessChain:
                                if (user->NumInOperands() >= 2 &&
                                    user->GetSingleWordInOperand(0) == variableId) {
                                    chainsByStruct[structId].push_back(user);
                                    return;
                                }
                                undoableStructs.insert(structId);
                                return;
                            default:
                                undoableStructs.insert(structId);
                                return;
                            }
                        });
                    }

                    for (const auto& [structId, structType] : candidateStructs) {
                        if (undoableStructs.count(structId) != 0) continue;

                        BlockPlan plan;
                        plan.structType = structType;
                        const uint32_t memberCount = structType->NumInOperands();
                        bool expressible = true;
                        for (uint32_t member = 0; member < memberCount; ++member) {
                            uint32_t byteOffset = 0;
                            if (!TryGetMemberDecorationLiteral(context, structId, member,
                                                               spv::Decoration::Offset, &byteOffset) ||
                                byteOffset % kCounterBytes != 0u) {
                                expressible = false;
                                break;
                            }
                            MemberPlan memberPlan;
                            uint32_t uintTypeId = 0;
                            if (!DescribeMember(context, structType->GetSingleWordInOperand(member), &uintTypeId,
                                                &memberPlan)) {
                                expressible = false;
                                break;
                            }
                            if (plan.uintTypeId != 0 && plan.uintTypeId != uintTypeId) {
                                expressible = false;
                                break;
                            }
                            plan.uintTypeId = uintTypeId;
                            memberPlan.elementOffset = byteOffset / kCounterBytes;
                            const uint64_t end = static_cast<uint64_t>(memberPlan.elementOffset) +
                                                 static_cast<uint64_t>(memberPlan.elementCount);
                            if (end > kMaxCounterElements) {
                                expressible = false;
                                break;
                            }
                            if (end > plan.totalElements) plan.totalElements = static_cast<uint32_t>(end);
                            plan.members.push_back(memberPlan);
                        }
                        if (!expressible || plan.members.empty() || plan.totalElements == 0) continue;
                        // Already std430: leave it exactly as it is. This is the overwhelmingly
                        // common answer and the reason the pass can be gated on a cheap probe.
                        if (IsNaturallyPacked(plan.members)) continue;

                        // Every chain must be one of the two shapes the re-index understands: a
                        // scalar counter reached by (variable, member) or an array element
                        // reached by (variable, member, index). One that stops at the member, or
                        // reaches deeper, is not a counter access this pass can move.
                        const auto chains = chainsByStruct.find(structId);
                        if (chains != chainsByStruct.end()) {
                            for (Instruction* chain : chains->second) {
                                const spvtools::opt::analysis::Constant* memberIndex =
                                    context->get_constant_mgr()->FindDeclaredConstant(
                                        chain->GetSingleWordInOperand(1));
                                if (memberIndex == nullptr || memberIndex->AsIntConstant() == nullptr) {
                                    expressible = false;
                                    break;
                                }
                                const uint32_t member = memberIndex->AsIntConstant()->GetU32BitValue();
                                if (member >= plan.members.size() ||
                                    chain->NumInOperands() != (plan.members[member].isArray ? 3u : 2u)) {
                                    expressible = false;
                                    break;
                                }
                                plan.chains.push_back(chain);
                            }
                        }
                        if (!expressible) continue;

                        plans.push_back(std::move(plan));
                    }
                    return plans;
                }

                // The id of |value| as a constant of the same integer type as |likeId|.
                uint32_t ConstantLike(IRContext* context, uint32_t likeId, uint32_t value) {
                    Instruction* likeDef = context->get_def_use_mgr()->GetDef(likeId);
                    const spvtools::opt::analysis::Type* type =
                        context->get_type_mgr()->GetType(likeDef->type_id());
                    const spvtools::opt::analysis::Constant* constant =
                        context->get_constant_mgr()->GetConstant(type, {value});
                    return context->get_constant_mgr()->GetDefiningInstruction(constant)->result_id();
                }

                Module::inst_iterator PositionOf(IRContext* context, const Instruction* target) {
                    for (auto it = context->types_values_begin(); it != context->types_values_end(); ++it) {
                        if (&*it == target) return it;
                    }
                    return context->types_values_end();
                }

                // Whether |firstId| is declared before |secondId| in the types/constants section.
                bool DeclaredBefore(IRContext* context, uint32_t firstId, uint32_t secondId) {
                    for (const Instruction& inst : context->module()->types_values()) {
                        if (inst.result_id() == firstId) return true;
                        if (inst.result_id() == secondId) return false;
                    }
                    return false;
                }

                // A fresh `uint[length]` with ArrayStride 4, spliced in immediately BEFORE the
                // block that will name it - SPIR-V has no forward references between types, so
                // appending it at the end of the section would make the module invalid. A
                // duplicate OpTypeArray is legal (SPIR-V 2.8 exempts aggregates from the
                // uniqueness rule, and so does spirv-val), so no search for an existing one is
                // needed; the LENGTH CONSTANT is not exempt, so when the module already declares
                // it the pass has to work with the one instruction that exists.
                //
                // That instruction is not always in a usable place. GetDefiningInstruction only
                // honours `position` when it MINTS the constant; when the module already has one
                // it hands back the existing instruction wherever it happens to sit, and glslang
                // emits constants in first-use order, so a shader whose first use of the value is
                // below the counter block declares it below the block. The flattened array would
                // then forward-reference its own length.
                //
                // KHR-GL43.compute_shader.pipeline-compute-chain is exactly that shader: two
                // counters at offset 8 need a 4-element array, and its `%uint_4` is first used by
                // a later declaration, so it lands AFTER gl_AtomicCounterBlock_1. Declining there
                // - which is what this used to do - left the offsets in place, and SPIRV-Cross
                // then refused the whole stage with "Push constant block cannot be expressed as
                // neither std430 nor std140", so the chain's first kernel never reached the
                // driver and every resource it writes stayed at its initial value.
                //
                // Moving the constant UP to just before the block is always legal, which is why
                // this is a relocation and not a second declaration: an OpConstant's only operand
                // is its result TYPE, and that type already precedes the block (it is the element
                // type of the counter array the block declares). Every existing use sits after
                // the constant's old position and therefore after its new one too, so no use is
                // left dangling - moving a definition earlier in the types/constants section
                // cannot invalidate anything. Ordering is all that changes; def-use is untouched.
                uint32_t CreateCounterArrayTypeBefore(IRContext* context, Instruction* structType,
                                                      uint32_t uintTypeId, uint32_t length) {
                    auto* constantMgr = context->get_constant_mgr();
                    const spvtools::opt::analysis::Type* uintType = context->get_type_mgr()->GetType(uintTypeId);
                    if (uintType == nullptr) return 0;
                    const spvtools::opt::analysis::Constant* lengthConstant =
                        constantMgr->GetConstant(uintType, {length});
                    if (lengthConstant == nullptr) return 0;

                    Module::inst_iterator position = PositionOf(context, structType);
                    if (position == context->types_values_end()) return 0;
                    Instruction* lengthInst = constantMgr->GetDefiningInstruction(lengthConstant, 0, &position);
                    if (lengthInst == nullptr) return 0;
                    if (!DeclaredBefore(context, lengthInst->result_id(), structType->result_id())) {
                        // Pre-existing constant, declared below the block. Relocate it; see above
                        // for why that is sound. InsertBefore unlinks it from its current spot
                        // first, so this is a move rather than an aliasing second entry.
                        lengthInst->InsertBefore(structType);
                    }

                    const uint32_t arrayTypeId = context->TakeNextId();
                    if (arrayTypeId == 0) return 0;
                    auto arrayType = MakeUnique<Instruction>(
                        context, spv::Op::OpTypeArray, 0, arrayTypeId,
                        std::initializer_list<Operand>{{SPV_OPERAND_TYPE_ID, {uintTypeId}},
                                                       {SPV_OPERAND_TYPE_ID, {lengthInst->result_id()}}});
                    Instruction* inserted = structType->InsertBefore(std::move(arrayType));
                    context->AnalyzeDefUse(inserted);
                    context->get_decoration_mgr()->AddDecorationVal(
                        arrayTypeId, static_cast<uint32_t>(spv::Decoration::ArrayStride), kCounterBytes);
                    return arrayTypeId;
                }

                // Drops the annotations the collapsed struct no longer has a member for: every
                // OpMemberDecorate and OpMemberName past member 0, plus member 0's own Offset
                // (the caller re-adds it as 0). Member 0's OTHER decorations - Coherent,
                // Volatile, Restrict and the like, which describe how the counters are accessed
                // rather than where they sit - are deliberately kept.
                void StripMemberAnnotations(IRContext* context, uint32_t structId) {
                    std::vector<Instruction*> doomed;
                    for (Instruction* decoration :
                         context->get_decoration_mgr()->GetDecorationsFor(structId, false)) {
                        if (decoration->opcode() != spv::Op::OpMemberDecorate ||
                            decoration->NumInOperands() < 3) {
                            continue;
                        }
                        const bool pastMemberZero = decoration->GetSingleWordInOperand(1) != 0u;
                        const bool isOffset =
                            static_cast<spv::Decoration>(decoration->GetSingleWordInOperand(2)) ==
                            spv::Decoration::Offset;
                        if (pastMemberZero || isOffset) doomed.push_back(decoration);
                    }
                    for (Instruction& debug : context->module()->debugs2()) {
                        if (debug.opcode() != spv::Op::OpMemberName || debug.NumInOperands() < 2) continue;
                        if (debug.GetSingleWordInOperand(0) != structId) continue;
                        if (debug.GetSingleWordInOperand(1) == 0u) continue; // member 0 keeps its name
                        doomed.push_back(&debug);
                    }
                    for (Instruction* inst : doomed) context->KillInst(inst);
                }
            } // namespace

            bool FlattenAtomicCounterBlockPass::BinaryHasOffsetAtomicCounterBlock(const Vector<Uint32>& binary) {
                if (binary.empty()) {
                    return false;
                }
                std::unique_ptr<IRContext> context = spvtools::BuildModule(
                    SPV_ENV_VULKAN_1_1,
                    [](spv_message_level_t, const char*, const spv_position_t&, const char*) {},
                    binary.data(), binary.size());
                if (!context) {
                    // Unparseable here means unusable downstream too; let the ordinary transpile
                    // path produce the error rather than inventing a verdict from it.
                    return false;
                }
                return !BuildPlans(context.get()).empty();
            }

            spvtools::opt::Pass::Status FlattenAtomicCounterBlockPass::Process() {
                auto* irContext = context();
                const std::vector<BlockPlan> plans = BuildPlans(irContext);
                if (plans.empty()) {
                    return Status::SuccessWithoutChange;
                }

                bool modified = false;
                for (const BlockPlan& plan : plans) {
                    const uint32_t structId = plan.structType->result_id();
                    const uint32_t arrayTypeId = CreateCounterArrayTypeBefore(
                        irContext, plan.structType, plan.uintTypeId, plan.totalElements);
                    if (arrayTypeId == 0) {
                        MGLOG_D("[spirv] atomic-counter block %%%u: no legal place for the flattened array "
                                "type; leaving the block alone",
                                structId);
                        continue;
                    }

                    // Re-index BEFORE the struct is collapsed, so the member index each chain
                    // carries still names the member the plan was built from.
                    for (Instruction* chain : plan.chains) {
                        const uint32_t memberIndexId = chain->GetSingleWordInOperand(1);
                        const uint32_t member = irContext->get_constant_mgr()
                                                    ->FindDeclaredConstant(memberIndexId)
                                                    ->AsIntConstant()
                                                    ->GetU32BitValue();
                        const MemberPlan& memberPlan = plan.members[member];

                        std::vector<Operand> operands;
                        operands.push_back(chain->GetInOperand(0));
                        operands.push_back({SPV_OPERAND_TYPE_ID, {ConstantLike(irContext, memberIndexId, 0u)}});
                        if (!memberPlan.isArray) {
                            operands.push_back(
                                {SPV_OPERAND_TYPE_ID,
                                 {ConstantLike(irContext, memberIndexId, memberPlan.elementOffset)}});
                        } else {
                            const uint32_t elementId = chain->GetSingleWordInOperand(2);
                            uint32_t shiftedId = elementId;
                            if (memberPlan.elementOffset != 0u) {
                                const spvtools::opt::analysis::Constant* elementConstant =
                                    irContext->get_constant_mgr()->FindDeclaredConstant(elementId);
                                if (elementConstant != nullptr && elementConstant->AsIntConstant() != nullptr) {
                                    shiftedId = ConstantLike(irContext, elementId,
                                                             elementConstant->AsIntConstant()->GetU32BitValue() +
                                                                 memberPlan.elementOffset);
                                } else {
                                    InstructionBuilder builder(
                                        irContext, chain,
                                        IRContext::kAnalysisDefUse | IRContext::kAnalysisInstrToBlockMapping);
                                    Instruction* elementDef = irContext->get_def_use_mgr()->GetDef(elementId);
                                    shiftedId = builder
                                                    .AddBinaryOp(elementDef->type_id(), spv::Op::OpIAdd,
                                                                 elementId,
                                                                 ConstantLike(irContext, elementId,
                                                                              memberPlan.elementOffset))
                                                    ->result_id();
                                }
                            }
                            operands.push_back({SPV_OPERAND_TYPE_ID, {shiftedId}});
                        }
                        chain->SetInOperands(std::move(operands));
                        irContext->UpdateDefUse(chain);
                    }

                    StripMemberAnnotations(irContext, structId);
                    plan.structType->SetInOperands({{SPV_OPERAND_TYPE_ID, {arrayTypeId}}});
                    irContext->UpdateDefUse(plan.structType);
                    irContext->get_decoration_mgr()->AddMemberDecoration(
                        structId, 0u, static_cast<uint32_t>(spv::Decoration::Offset), 0u);
                    modified = true;
                    MGLOG_D("[spirv] atomic-counter block %%%u: collapsed %zu offset member(s) into one "
                            "%u-element array so std430 can express it",
                            structId, plan.members.size(), plan.totalElements);
                }

                if (!modified) {
                    return Status::SuccessWithoutChange;
                }
                irContext->InvalidateAnalysesExceptFor(IRContext::kAnalysisNone);
                return Status::SuccessWithChange;
            }

            spvtools::Optimizer::PassToken FlattenAtomicCounterBlockPass::CreateFlattenAtomicCounterBlockPass() {
                return spvtools::Optimizer::PassToken(MakeUnique<FlattenAtomicCounterBlockPass>());
            }
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
