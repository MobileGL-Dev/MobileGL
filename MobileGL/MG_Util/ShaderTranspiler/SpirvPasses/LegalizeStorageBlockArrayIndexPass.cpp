// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/LegalizeStorageBlockArrayIndexPass.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "LegalizeStorageBlockArrayIndexPass.h"

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
#include "source/opt/loop_descriptor.h"
#include "source/opt/module.h"
#include "source/opt/type_manager.h"
#include "source/util/make_unique.h"

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
                using spvtools::opt::Operand;

                // GL_MAX_*_SHADER_STORAGE_BLOCKS is 16 on the devices MobileGL targets, and
                // each lowered element costs one basic block per write, so a module claiming
                // more than this is refused rather than exploded. The largest array in the
                // conformance suite is 8.
                constexpr uint32_t kMaxLoweredArrayLength = 32;
                // One CFG-changing rewrite per round (analyses are dropped after each), so
                // the round budget bounds the work on a pathological module.
                constexpr int kMaxLoweringRounds = 256;
                // Full unrolling copies the body once per iteration, and nothing in the stock
                // unroller bounds that. Past this count the loop is left alone and the switch
                // lowering, whose cost is the array length rather than the trip count, takes
                // it instead. A loop over an array of storage blocks iterates at most
                // GL_MAX_*_SHADER_STORAGE_BLOCKS times in any shader that is not already
                // broken.
                constexpr size_t kMaxUnrolledIterations = 64;

                struct DynamicIndexUse {
                    Instruction* accessChain = nullptr;
                    uint32_t arrayLength = 0;
                };

                bool HasDecoration(IRContext* context, uint32_t id, spv::Decoration kind) {
                    for (Instruction* decoration : context->get_decoration_mgr()->GetDecorationsFor(id, false)) {
                        if (decoration->opcode() != spv::Op::OpDecorate ||
                            decoration->NumInOperands() < 2) {
                            continue;
                        }
                        if (static_cast<spv::Decoration>(decoration->GetSingleWordInOperand(1)) == kind) {
                            return true;
                        }
                    }
                    return false;
                }

                // Every variable that is an ARRAY OF STORAGE BLOCKS, mapped to that array's
                // length. Two spellings are accepted because both reach here depending on the
                // SPIR-V version glslang targets: StorageBuffer + Block (1.3, what MobileGL
                // asks for) and Uniform + BufferBlock (the pre-1.3 encoding). A UNIFORM block
                // array - Uniform + Block - is deliberately NOT collected; see the header.
                //
                // A length that is not a plain OpConstant (a spec constant) maps to 0: still
                // detected as illegal ESSL, never lowered.
                std::unordered_map<uint32_t, uint32_t> CollectStorageBlockArrays(IRContext* context) {
                    std::unordered_map<uint32_t, uint32_t> blockArrays;
                    auto* defUseMgr = context->get_def_use_mgr();
                    auto* constantMgr = context->get_constant_mgr();

                    for (Instruction& inst : context->module()->types_values()) {
                        if (inst.opcode() != spv::Op::OpVariable) {
                            continue;
                        }
                        const auto storageClass =
                            static_cast<spv::StorageClass>(inst.GetSingleWordInOperand(0));
                        if (storageClass != spv::StorageClass::StorageBuffer &&
                            storageClass != spv::StorageClass::Uniform) {
                            continue;
                        }

                        Instruction* pointerType = defUseMgr->GetDef(inst.type_id());
                        if (pointerType == nullptr || pointerType->opcode() != spv::Op::OpTypePointer) {
                            continue;
                        }
                        Instruction* pointeeType = defUseMgr->GetDef(pointerType->GetSingleWordInOperand(1));
                        if (pointeeType == nullptr || pointeeType->opcode() != spv::Op::OpTypeArray) {
                            continue;
                        }
                        Instruction* elementType = defUseMgr->GetDef(pointeeType->GetSingleWordInOperand(0));
                        if (elementType == nullptr || elementType->opcode() != spv::Op::OpTypeStruct) {
                            continue;
                        }

                        const bool isStorageBlock =
                            storageClass == spv::StorageClass::StorageBuffer
                                ? HasDecoration(context, elementType->result_id(), spv::Decoration::Block)
                                : HasDecoration(context, elementType->result_id(),
                                                spv::Decoration::BufferBlock);
                        if (!isStorageBlock) {
                            continue;
                        }

                        uint32_t arrayLength = 0;
                        const spvtools::opt::analysis::Constant* lengthConstant =
                            constantMgr->FindDeclaredConstant(pointeeType->GetSingleWordInOperand(1));
                        if (lengthConstant != nullptr && lengthConstant->AsIntConstant() != nullptr) {
                            arrayLength = lengthConstant->AsIntConstant()->GetU32BitValue();
                        }
                        blockArrays.emplace(inst.result_id(), arrayLength);
                    }
                    return blockArrays;
                }

                // "Constant integral expression" in the ESSL sense: an OpConstant (or the
                // zero an OpConstantNull stands for). A spec constant is deliberately NOT
                // one - SPIRV-Cross prints it as an identifier, which is exactly what the
                // driver rejects.
                bool IsConstantIndex(IRContext* context, uint32_t indexId) {
                    Instruction* def = context->get_def_use_mgr()->GetDef(indexId);
                    return def != nullptr && (def->opcode() == spv::Op::OpConstant ||
                                              def->opcode() == spv::Op::OpConstantNull);
                }

                // Access chains that index an array of storage blocks with a non-constant.
                // Only the FIRST index is considered: it is the one that selects the block,
                // and it is the only one ESSL constrains here. Indices inside the block - the
                // member selector and any array subscript below it - are legal however they
                // are computed, and chains rooted at another access chain are already inside
                // one element.
                std::vector<DynamicIndexUse> CollectDynamicIndexUses(IRContext* context) {
                    std::vector<DynamicIndexUse> uses;
                    const std::unordered_map<uint32_t, uint32_t> blockArrays =
                        CollectStorageBlockArrays(context);
                    if (blockArrays.empty()) {
                        return uses;
                    }

                    for (Function& function : *context->module()) {
                        for (BasicBlock& block : function) {
                            for (Instruction& inst : block) {
                                if (inst.opcode() != spv::Op::OpAccessChain &&
                                    inst.opcode() != spv::Op::OpInBoundsAccessChain) {
                                    continue;
                                }
                                if (inst.NumInOperands() < 2) {
                                    continue;
                                }
                                const auto arrayIt = blockArrays.find(inst.GetSingleWordInOperand(0));
                                if (arrayIt == blockArrays.end()) {
                                    continue;
                                }
                                if (IsConstantIndex(context, inst.GetSingleWordInOperand(1))) {
                                    continue;
                                }
                                uses.push_back({&inst, arrayIt->second});
                            }
                        }
                    }
                    return uses;
                }

                // The block index operand of |accessChain| replaced by the constant |element|,
                // built at the builder's insertion point. Every later index is copied through
                // unchanged: `arr[idx].data[j]` keeps its (legal) dynamic member subscript.
                Instruction* CloneChainWithConstantIndex(InstructionBuilder& builder, IRContext* context,
                                                         Instruction* accessChain, uint32_t constantIndexId) {
                    std::vector<Operand> operands;
                    operands.reserve(accessChain->NumInOperands());
                    for (uint32_t i = 0; i < accessChain->NumInOperands(); ++i) {
                        if (i == 1) {
                            operands.push_back({SPV_OPERAND_TYPE_ID, {constantIndexId}});
                        } else {
                            operands.push_back(accessChain->GetInOperand(i));
                        }
                    }
                    return builder.AddInstruction(MakeUnique<Instruction>(context, accessChain->opcode(),
                                                                          accessChain->type_id(),
                                                                          context->TakeNextId(), operands));
                }

                // The id of |element| as a constant of the same integer type as |indexId|.
                uint32_t ConstantLikeIndex(IRContext* context, uint32_t indexId, uint32_t element) {
                    Instruction* indexDef = context->get_def_use_mgr()->GetDef(indexId);
                    const spvtools::opt::analysis::Type* indexType =
                        context->get_type_mgr()->GetType(indexDef->type_id());
                    const spvtools::opt::analysis::Constant* constant =
                        context->get_constant_mgr()->GetConstant(indexType, {element});
                    return context->get_constant_mgr()->GetDefiningInstruction(constant)->result_id();
                }

                // A 32-bit integer is the only index this pass lowers: OpSwitch matches its
                // literals against the selector's width, and every ESSL block-array index is
                // an int or uint.
                bool IsLowerableIndexType(IRContext* context, uint32_t indexId) {
                    Instruction* indexDef = context->get_def_use_mgr()->GetDef(indexId);
                    if (indexDef == nullptr) {
                        return false;
                    }
                    const spvtools::opt::analysis::Type* type =
                        context->get_type_mgr()->GetType(indexDef->type_id());
                    const spvtools::opt::analysis::Integer* integer =
                        type != nullptr ? type->AsInteger() : nullptr;
                    return integer != nullptr && integer->width() == 32;
                }

                // The condition type OpSelect needs for |resultTypeId|. Before SPIR-V 1.4 a
                // scalar bool may not select between vectors, so a vector result needs a bool
                // vector of the same width - built by broadcasting the scalar comparison.
                // Anything that is neither scalar nor vector (a matrix or struct element) is
                // refused: pre-1.4 OpSelect cannot express it either.
                bool TryGetSelectConditionType(IRContext* context, uint32_t resultTypeId,
                                               uint32_t* conditionTypeId, uint32_t* dimension) {
                    auto* typeMgr = context->get_type_mgr();
                    const spvtools::opt::analysis::Type* resultType = typeMgr->GetType(resultTypeId);
                    if (resultType == nullptr) {
                        return false;
                    }

                    spvtools::opt::analysis::Bool boolType;
                    if (resultType->AsVector() != nullptr) {
                        const uint32_t count = resultType->AsVector()->element_count();
                        spvtools::opt::analysis::Vector boolVector(&boolType, count);
                        *conditionTypeId = typeMgr->GetTypeInstruction(&boolVector);
                        *dimension = count;
                        return *conditionTypeId != 0;
                    }
                    if (resultType->AsInteger() != nullptr || resultType->AsFloat() != nullptr ||
                        resultType->AsBool() != nullptr) {
                        *conditionTypeId = typeMgr->GetTypeInstruction(&boolType);
                        *dimension = 1;
                        return *conditionTypeId != 0;
                    }
                    return false;
                }

                // Whether fully unrolling |loop| is bounded work. The trip count is read the
                // same way the stock unroller reads it, so a loop this declines to measure is
                // one CanPerformUnroll would refuse anyway - the hint would be inert on it,
                // and the fallback lowering is what handles it. Requires the induction
                // variable to already be an OpPhi, which is why this runs after ssa-rewrite.
                bool IsBoundedUnrollCandidate(spvtools::opt::Loop* loop) {
                    const spvtools::opt::BasicBlock* condition = loop->FindConditionBlock();
                    if (condition == nullptr) {
                        return false;
                    }
                    const Instruction* induction = loop->FindConditionVariable(condition);
                    if (induction == nullptr || induction->opcode() != spv::Op::OpPhi) {
                        return false;
                    }
                    size_t iterations = 0;
                    if (!loop->FindNumberOfIterations(induction, &*condition->ctail(), &iterations)) {
                        return false;
                    }
                    return iterations <= kMaxUnrolledIterations;
                }
            } // namespace

            bool LegalizeStorageBlockArrayIndexPass::BinaryHasDynamicStorageBlockArrayIndexing(
                const std::vector<uint32_t>& binary) {
                if (binary.empty()) {
                    return false;
                }
                std::unique_ptr<IRContext> context = spvtools::BuildModule(
                    SPV_ENV_VULKAN_1_1,
                    [](spv_message_level_t, const char*, const spv_position_t&, const char*) {},
                    binary.data(), binary.size());
                if (!context) {
                    return false;
                }
                return !CollectDynamicIndexUses(context.get()).empty();
            }

            spvtools::opt::Pass::Status LegalizeStorageBlockArrayIndexPass::Process() {
                return m_mode == Mode::MarkLoopsForUnroll ? MarkLoopsForUnroll() : LowerToConstantSwitch();
            }

            spvtools::opt::Pass::Status LegalizeStorageBlockArrayIndexPass::MarkLoopsForUnroll() {
                auto* irContext = context();
                const std::vector<DynamicIndexUse> uses = CollectDynamicIndexUses(irContext);
                if (uses.empty()) {
                    return Status::SuccessWithoutChange;
                }

                bool modified = false;
                for (const DynamicIndexUse& use : uses) {
                    BasicBlock* block = irContext->get_instr_block(use.accessChain);
                    if (block == nullptr) {
                        continue;
                    }
                    Function* function = block->GetParent();
                    if (function == nullptr) {
                        continue;
                    }

                    spvtools::opt::LoopDescriptor* loops = irContext->GetLoopDescriptor(function);
                    for (spvtools::opt::Loop* loop = (*loops)[block->id()]; loop != nullptr;
                         loop = loop->GetParent()) {
                        if (!IsBoundedUnrollCandidate(loop)) {
                            continue;
                        }
                        Instruction* mergeInst = loop->GetHeaderBlock()->GetLoopMergeInst();
                        // Only a bare `None` control is promoted, and only when no extra
                        // literal (PartialCount, PeelCount, ...) follows it: the unroller
                        // tests the control word for equality with Unroll, so ORing the bit
                        // into a control that already carries something - DontUnroll above
                        // all - would neither unroll nor mean what it says.
                        if (mergeInst == nullptr || mergeInst->NumOperands() != 3 ||
                            mergeInst->GetSingleWordOperand(2) !=
                                static_cast<uint32_t>(spv::LoopControlMask::MaskNone)) {
                            continue;
                        }
                        mergeInst->SetOperand(
                            2, {static_cast<uint32_t>(spv::LoopControlMask::Unroll)});
                        modified = true;
                    }
                }

                if (!modified) {
                    return Status::SuccessWithoutChange;
                }
                MGLOG_D("[spirv] storage-block array index: marked enclosing loops for full unrolling");
                return Status::SuccessWithChange;
            }

            spvtools::opt::Pass::Status LegalizeStorageBlockArrayIndexPass::LowerToConstantSwitch() {
                auto* irContext = context();

                bool modified = false;
                // Access chains this pass has already refused, so a shape it cannot rewrite
                // exactly cannot spin the round loop.
                std::unordered_set<uint32_t> declined;

                for (int round = 0; round < kMaxLoweringRounds; ++round) {
                    const std::vector<DynamicIndexUse> uses = CollectDynamicIndexUses(irContext);
                    bool progressed = false;

                    for (const DynamicIndexUse& use : uses) {
                        if (declined.count(use.accessChain->result_id()) != 0) {
                            continue;
                        }
                        const LoweringOutcome outcome = LowerOneChain(use.accessChain, use.arrayLength);
                        if (outcome == LoweringOutcome::Declined) {
                            declined.insert(use.accessChain->result_id());
                            continue;
                        }
                        if (outcome == LoweringOutcome::Changed) {
                            modified = true;
                            progressed = true;
                            // A store rewrite splits the block it sat in; every cached
                            // analysis (and the instruction list this loop is walking) is
                            // stale from here on. Recollect from scratch.
                            break;
                        }
                    }

                    if (!progressed) {
                        break;
                    }
                }

                if (!modified) {
                    return Status::SuccessWithoutChange;
                }
                return Status::SuccessWithChange;
            }

            LegalizeStorageBlockArrayIndexPass::LoweringOutcome
            LegalizeStorageBlockArrayIndexPass::LowerOneChain(Instruction* accessChain, uint32_t arrayLength) {
                auto* irContext = context();
                if (arrayLength == 0 || arrayLength > kMaxLoweredArrayLength) {
                    MGLOG_D("[spirv] storage-block array index: array length %u is not lowerable",
                            arrayLength);
                    return LoweringOutcome::Declined;
                }
                if (!IsLowerableIndexType(irContext, accessChain->GetSingleWordInOperand(1))) {
                    return LoweringOutcome::Declined;
                }

                std::vector<Instruction*> stores;
                std::vector<Instruction*> loads;
                bool unsupportedUse = false;
                irContext->get_def_use_mgr()->ForEachUser(accessChain, [&](Instruction* user) {
                    switch (user->opcode()) {
                    case spv::Op::OpName:
                    case spv::Op::OpDecorate:
                    case spv::Op::OpDecorateId:
                        return;
                    case spv::Op::OpStore:
                        // Only as the pointer. A pointer stored as a *value* is not a storage
                        // block write and cannot be redirected element-wise.
                        if (user->GetSingleWordInOperand(0) == accessChain->result_id()) {
                            stores.push_back(user);
                        } else {
                            unsupportedUse = true;
                        }
                        return;
                    case spv::Op::OpLoad:
                        // Memory operands (Volatile, Aligned, ...) would be dropped by the
                        // per-element rebuild, so a load carrying any is refused instead.
                        if (user->NumInOperands() == 1) {
                            loads.push_back(user);
                        } else {
                            unsupportedUse = true;
                        }
                        return;
                    default:
                        // A pointer passed to a function, copied, chained further, used by an
                        // atomic, or measured by OpArrayLength cannot be resolved to one
                        // element here.
                        unsupportedUse = true;
                        return;
                    }
                });

                if (unsupportedUse) {
                    MGLOG_D("[spirv] storage-block array index: chain %%%u has a use this pass cannot "
                            "rewrite",
                            accessChain->result_id());
                    return LoweringOutcome::Declined;
                }

                if (!loads.empty()) {
                    return LowerLoad(accessChain, arrayLength, loads.front());
                }
                if (!stores.empty()) {
                    return LowerStore(accessChain, arrayLength, stores.front());
                }

                // No uses left: the chain itself is what detection is still seeing.
                irContext->KillInst(accessChain);
                irContext->InvalidateAnalysesExceptFor(IRContext::kAnalysisNone);
                return LoweringOutcome::Changed;
            }

            // switch (idx) { case 0: arr[0]... = v; break; case 1: arr[1]... = v; break; ... }
            //
            // The block holding the store is split at the store, and the tail becomes the
            // switch's merge block, so whatever followed the store still runs exactly once on
            // every path. An index outside [0, length) reaches the default target, which is
            // the merge block: nothing is stored, which is what indexing a block array out of
            // range already meant.
            LegalizeStorageBlockArrayIndexPass::LoweringOutcome
            LegalizeStorageBlockArrayIndexPass::LowerStore(Instruction* accessChain, uint32_t arrayLength,
                                                           Instruction* store) {
                auto* irContext = context();
                BasicBlock* block = irContext->get_instr_block(store);
                if (block == nullptr) {
                    return LoweringOutcome::Declined;
                }
                // Splitting a loop header keeps the label - and so the back edge's target -
                // on the first half while the OpLoopMerge moves to the second, which is not
                // a loop any more. Refuse instead of producing that.
                if (block->GetLoopMergeInst() != nullptr) {
                    MGLOG_D("[spirv] storage-block array index: store sits in a loop header, declining");
                    return LoweringOutcome::Declined;
                }
                Function* function = block->GetParent();
                if (function == nullptr) {
                    return LoweringOutcome::Declined;
                }

                const uint32_t indexId = accessChain->GetSingleWordInOperand(1);
                const uint32_t valueId = store->GetSingleWordInOperand(1);
                std::vector<Operand> memoryOperands;
                for (uint32_t i = 2; i < store->NumInOperands(); ++i) {
                    memoryOperands.push_back(store->GetInOperand(i));
                }

                const uint32_t mergeLabelId = irContext->TakeNextId();
                block->SplitBasicBlock(irContext, mergeLabelId, BasicBlock::iterator(store));
                // |store| now heads the merge block; the per-element stores replace it.
                irContext->KillInst(store);

                std::vector<std::pair<Operand::OperandData, uint32_t>> targets;
                targets.reserve(arrayLength);
                BasicBlock* insertAfter = block;
                for (uint32_t element = 0; element < arrayLength; ++element) {
                    const uint32_t caseLabelId = irContext->TakeNextId();
                    auto caseBlock = MakeUnique<BasicBlock>(MakeUnique<Instruction>(
                        irContext, spv::Op::OpLabel, 0, caseLabelId, std::initializer_list<Operand>{}));
                    caseBlock->SetParent(function);
                    BasicBlock* casePtr = function->InsertBasicBlockAfter(std::move(caseBlock), insertAfter);
                    // The builders below register what they add, but this label was built by
                    // hand: without this the OpSwitch would name a target the def-use manager
                    // has never seen, which a consistency-checking build calls out.
                    irContext->AnalyzeDefUse(casePtr->GetLabelInst());
                    irContext->set_instr_block(casePtr->GetLabelInst(), casePtr);

                    InstructionBuilder caseBuilder(
                        irContext, casePtr,
                        IRContext::kAnalysisDefUse | IRContext::kAnalysisInstrToBlockMapping);
                    const uint32_t constantId = ConstantLikeIndex(irContext, indexId, element);
                    Instruction* elementChain =
                        CloneChainWithConstantIndex(caseBuilder, irContext, accessChain, constantId);

                    std::vector<Operand> storeOperands;
                    storeOperands.push_back({SPV_OPERAND_TYPE_ID, {elementChain->result_id()}});
                    storeOperands.push_back({SPV_OPERAND_TYPE_ID, {valueId}});
                    for (const Operand& memoryOperand : memoryOperands) {
                        storeOperands.push_back(memoryOperand);
                    }
                    caseBuilder.AddInstruction(
                        MakeUnique<Instruction>(irContext, spv::Op::OpStore, 0, 0, storeOperands));
                    caseBuilder.AddBranch(mergeLabelId);

                    targets.push_back({Operand::OperandData{element}, caseLabelId});
                    insertAfter = casePtr;
                }

                InstructionBuilder switchBuilder(
                    irContext, block, IRContext::kAnalysisDefUse | IRContext::kAnalysisInstrToBlockMapping);
                switchBuilder.AddSwitch(indexId, mergeLabelId, targets, mergeLabelId);

                if (irContext->get_def_use_mgr()->NumUsers(accessChain) == 0) {
                    irContext->KillInst(accessChain);
                }
                irContext->InvalidateAnalysesExceptFor(IRContext::kAnalysisNone);
                MGLOG_D("[spirv] storage-block array index: lowered a dynamic write to a %u-way switch",
                        arrayLength);
                return LoweringOutcome::Changed;
            }

            // A read needs no control flow: load every element through a constant index and
            // pick with OpSelect. Reading the elements the shader did not ask for is safe -
            // every one of them is a storage block this stage already declares, and an ES
            // driver bounds-checks a storage buffer read that lands outside what is bound.
            LegalizeStorageBlockArrayIndexPass::LoweringOutcome
            LegalizeStorageBlockArrayIndexPass::LowerLoad(Instruction* accessChain, uint32_t arrayLength,
                                                          Instruction* load) {
                auto* irContext = context();
                uint32_t conditionTypeId = 0;
                uint32_t dimension = 0;
                if (!TryGetSelectConditionType(irContext, load->type_id(), &conditionTypeId, &dimension)) {
                    MGLOG_D("[spirv] storage-block array index: element type is not selectable, declining");
                    return LoweringOutcome::Declined;
                }
                const uint32_t boolTypeId = irContext->get_type_mgr()->GetBoolTypeId();
                const uint32_t indexId = accessChain->GetSingleWordInOperand(1);

                InstructionBuilder builder(
                    irContext, load, IRContext::kAnalysisDefUse | IRContext::kAnalysisInstrToBlockMapping);

                uint32_t selectedId = 0;
                for (uint32_t element = 0; element < arrayLength; ++element) {
                    const uint32_t constantId = ConstantLikeIndex(irContext, indexId, element);
                    Instruction* elementChain =
                        CloneChainWithConstantIndex(builder, irContext, accessChain, constantId);
                    Instruction* elementLoad = builder.AddLoad(load->type_id(), elementChain->result_id());
                    if (element == 0) {
                        // Element 0 is the else-arm of the whole ladder, so an out-of-range
                        // index reads it - an undefined element for an undefined index.
                        selectedId = elementLoad->result_id();
                        continue;
                    }

                    Instruction* isElement =
                        builder.AddBinaryOp(boolTypeId, spv::Op::OpIEqual, indexId, constantId);
                    uint32_t conditionId = isElement->result_id();
                    if (dimension > 1) {
                        std::vector<uint32_t> components(dimension, conditionId);
                        conditionId = builder.AddCompositeConstruct(conditionTypeId, components)->result_id();
                    }
                    selectedId = builder
                                     .AddSelect(load->type_id(), conditionId, elementLoad->result_id(),
                                                selectedId)
                                     ->result_id();
                }

                irContext->ReplaceAllUsesWith(load->result_id(), selectedId);
                irContext->KillInst(load);
                irContext->InvalidateAnalysesExceptFor(IRContext::kAnalysisNone);
                MGLOG_D("[spirv] storage-block array index: lowered a dynamic read to %u constant-indexed "
                        "loads",
                        arrayLength);
                return LoweringOutcome::Changed;
            }

            spvtools::Optimizer::PassToken
            LegalizeStorageBlockArrayIndexPass::CreateMarkLoopsForUnrollPass() {
                return spvtools::Optimizer::PassToken(
                    MakeUnique<LegalizeStorageBlockArrayIndexPass>(Mode::MarkLoopsForUnroll));
            }

            spvtools::Optimizer::PassToken
            LegalizeStorageBlockArrayIndexPass::CreateLowerToConstantSwitchPass() {
                return spvtools::Optimizer::PassToken(
                    MakeUnique<LegalizeStorageBlockArrayIndexPass>(Mode::LowerToConstantSwitch));
            }
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
