// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/LegalizeResourceArrayIndexPass.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "LegalizeResourceArrayIndexPass.h"

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

                // GL_MAX_*_SHADER_STORAGE_BLOCKS and GL_MAX_*_IMAGE_UNIFORMS are both 16 or
                // fewer on the devices MobileGL targets, and each lowered element costs one
                // basic block per write, so a module claiming more than this is refused rather
                // than exploded. The largest array in the conformance suite is 8.
                constexpr uint32_t kMaxLoweredArrayLength = 32;
                // One CFG-changing rewrite per round (analyses are dropped after each), so
                // the round budget bounds the work on a pathological module.
                constexpr int kMaxLoweringRounds = 256;
                // Full unrolling copies the body once per iteration, and nothing in the stock
                // unroller bounds that. Past this many copies the nest is left alone and the
                // switch lowering, whose cost is the array length rather than the trip count,
                // takes it instead. A loop over an array of storage blocks or of images iterates
                // at most GL_MAX_*_SHADER_STORAGE_BLOCKS / GL_MAX_*_IMAGE_UNIFORMS times in any
                // shader that is not already broken.
                //
                // This is a budget for the whole NEST, not for one loop: marking a loop for
                // unrolling means marking its ancestors too (see MarkLoopsForUnroll), and the
                // copies they produce multiply.
                constexpr size_t kMaxUnrolledIterations = 64;

                struct ResourceArray {
                    uint32_t length = 0;
                    // Which lowering the chain's uses need; see the header. Detection and
                    // loop-marking are identical for both.
                    bool isImage = false;
                };

                struct DynamicIndexUse {
                    Instruction* accessChain = nullptr;
                    uint32_t arrayLength = 0;
                    bool isImageArray = false;
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

                // Every variable that is an ARRAY OF STORAGE BLOCKS or an ARRAY OF IMAGE
                // UNIFORMS, mapped to that array's length and kind.
                //
                // Storage blocks: two spellings are accepted because both reach here depending
                // on the SPIR-V version glslang targets: StorageBuffer + Block (1.3, what
                // MobileGL asks for) and Uniform + BufferBlock (the pre-1.3 encoding). A UNIFORM
                // block array - Uniform + Block - is deliberately NOT collected; see the header.
                //
                // Images: UniformConstant + OpTypeArray of OpTypeImage. Sampled == 2 is what
                // separates a storage image - what GLSL calls `image2D` and what the ES rule is
                // about - from the OpTypeImage that sits INSIDE an OpTypeSampledImage, which
                // never appears as an array element type on its own here and whose array ESSL
                // 3.20 4.1.7 explicitly permits a dynamically-uniform index.
                //
                // A length that is not a plain OpConstant (a spec constant) maps to 0: still
                // detected as illegal ESSL, never lowered.
                std::unordered_map<uint32_t, ResourceArray> CollectResourceArrays(IRContext* context) {
                    std::unordered_map<uint32_t, ResourceArray> resourceArrays;
                    auto* defUseMgr = context->get_def_use_mgr();
                    auto* constantMgr = context->get_constant_mgr();

                    for (Instruction& inst : context->module()->types_values()) {
                        if (inst.opcode() != spv::Op::OpVariable) {
                            continue;
                        }
                        const auto storageClass =
                            static_cast<spv::StorageClass>(inst.GetSingleWordInOperand(0));
                        if (storageClass != spv::StorageClass::StorageBuffer &&
                            storageClass != spv::StorageClass::Uniform &&
                            storageClass != spv::StorageClass::UniformConstant) {
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
                        if (elementType == nullptr) {
                            continue;
                        }

                        bool isImage = false;
                        if (storageClass == spv::StorageClass::UniformConstant) {
                            // OpTypeImage <result> <sampled type> <dim> <depth> <arrayed> <ms>
                            //             <sampled> <format>
                            if (elementType->opcode() != spv::Op::OpTypeImage ||
                                elementType->NumInOperands() < 6 ||
                                elementType->GetSingleWordInOperand(5) != 2u) {
                                continue;
                            }
                            isImage = true;
                        } else {
                            if (elementType->opcode() != spv::Op::OpTypeStruct) {
                                continue;
                            }
                            const bool isStorageBlock =
                                storageClass == spv::StorageClass::StorageBuffer
                                    ? HasDecoration(context, elementType->result_id(),
                                                    spv::Decoration::Block)
                                    : HasDecoration(context, elementType->result_id(),
                                                    spv::Decoration::BufferBlock);
                            if (!isStorageBlock) {
                                continue;
                            }
                        }

                        uint32_t arrayLength = 0;
                        const spvtools::opt::analysis::Constant* lengthConstant =
                            constantMgr->FindDeclaredConstant(pointeeType->GetSingleWordInOperand(1));
                        if (lengthConstant != nullptr && lengthConstant->AsIntConstant() != nullptr) {
                            arrayLength = lengthConstant->AsIntConstant()->GetU32BitValue();
                        }
                        resourceArrays.emplace(inst.result_id(), ResourceArray{arrayLength, isImage});
                    }
                    return resourceArrays;
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

                // Access chains that index an array of storage blocks or of images with a
                // non-constant. Only the FIRST index is considered: it is the one that selects
                // the element, and it is the only one ESSL constrains here. Indices inside the
                // block - the member selector and any array subscript below it - are legal
                // however they are computed, and chains rooted at another access chain are
                // already inside one element.
                std::vector<DynamicIndexUse> CollectDynamicIndexUses(IRContext* context) {
                    std::vector<DynamicIndexUse> uses;
                    const std::unordered_map<uint32_t, ResourceArray> resourceArrays =
                        CollectResourceArrays(context);
                    if (resourceArrays.empty()) {
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
                                const auto arrayIt = resourceArrays.find(inst.GetSingleWordInOperand(0));
                                if (arrayIt == resourceArrays.end()) {
                                    continue;
                                }
                                if (IsConstantIndex(context, inst.GetSingleWordInOperand(1))) {
                                    continue;
                                }
                                uses.push_back(
                                    {&inst, arrayIt->second.length, arrayIt->second.isImage});
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

                // |loop|'s trip count, when it has a measurable one, in *outIterations. The
                // count is read the same way the stock unroller reads it, so a loop this
                // declines to measure is one CanPerformUnroll would refuse anyway - the hint
                // would be inert on it, and the fallback lowering is what handles it. Requires
                // the induction variable to already be an OpPhi, which is why this runs after
                // ssa-rewrite.
                //
                // A count of zero is reported as unmeasurable: it means nothing this pass can
                // multiply a nest's budget by, and a loop that never runs is not one whose
                // subscript needs folding.
                bool TryGetUnrollTripCount(spvtools::opt::Loop* loop, size_t* outIterations) {
                    const spvtools::opt::BasicBlock* condition = loop->FindConditionBlock();
                    if (condition == nullptr) {
                        return false;
                    }
                    const Instruction* induction = loop->FindConditionVariable(condition);
                    if (induction == nullptr || induction->opcode() != spv::Op::OpPhi) {
                        return false;
                    }
                    size_t iterations = 0;
                    if (!loop->FindNumberOfIterations(induction, &*condition->ctail(), &iterations) ||
                        iterations == 0) {
                        return false;
                    }
                    *outIterations = iterations;
                    return true;
                }
            } // namespace

            bool LegalizeResourceArrayIndexPass::BinaryHasDynamicResourceArrayIndexing(
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

            spvtools::opt::Pass::Status LegalizeResourceArrayIndexPass::Process() {
                return m_mode == Mode::MarkLoopsForUnroll ? MarkLoopsForUnroll() : LowerToConstantSwitch();
            }

            spvtools::opt::Pass::Status LegalizeResourceArrayIndexPass::MarkLoopsForUnroll() {
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

                    // The offending chain's own loop AND every loop enclosing it, because
                    // SPIRV-Tools only ever unrolls an INNERMOST loop - an outer one is
                    // unrollable only once its children are gone - and because the index that
                    // has to become a literal may be an outer loop's induction variable.
                    //
                    // Marking a whole nest means the unrolled body count is the PRODUCT of its
                    // trip counts, not the largest of them, so the budget is spent as the walk
                    // climbs rather than tested loop by loop. Measured through
                    // LegalizeResourceArrayIndexingForEssl itself, on a twelve-line shader
                    // writing image2D g_image[4] from a 64/64/4 nest - every level individually
                    // inside the per-loop cap, which is all this used to test: 256 OpImageWrite
                    // with the per-loop cap alone against 4 with the nest budget, and the
                    // per-loop cap bounds nothing at all as the trip counts grow. The image half
                    // of this pass is what made such a nest reachable; a storage-block array
                    // rarely sits inside one. BoundsTheWholeLoopNestAndNotEachLoopSeparately is
                    // that measurement.
                    //
                    // Every exit is a BREAK rather than a skip-and-keep-climbing. A loop that
                    // cannot be marked - unmeasurable, out of budget, or carrying a control this
                    // pass will not overwrite - is a gap the unroller cannot cross, which makes
                    // every mark above it dead weight on a module that will not be unrolled
                    // anyway. Falling out of the unroll path costs nothing correctness-wise:
                    // LowerToConstantSwitch still legalizes the chain, at a cost proportional to
                    // the ARRAY LENGTH rather than to the trip counts.
                    spvtools::opt::LoopDescriptor* loops = irContext->GetLoopDescriptor(function);
                    size_t nestIterations = 1;
                    for (spvtools::opt::Loop* loop = (*loops)[block->id()]; loop != nullptr;
                         loop = loop->GetParent()) {
                        size_t iterations = 0;
                        if (!TryGetUnrollTripCount(loop, &iterations)) {
                            break;
                        }
                        // Division, not multiplication, so the test itself cannot overflow.
                        if (iterations > kMaxUnrolledIterations / nestIterations) {
                            break;
                        }
                        Instruction* mergeInst = loop->GetHeaderBlock()->GetLoopMergeInst();
                        // Only a bare `None` control is promoted, and only when no extra
                        // literal (PartialCount, PeelCount, ...) follows it: the unroller
                        // tests the control word for equality with Unroll, so ORing the bit
                        // into a control that already carries something - DontUnroll above
                        // all - would neither unroll nor mean what it says. An `Unroll` this
                        // pass itself already wrote for another chain in the same nest ends the
                        // walk too: everything above it was considered on that pass through.
                        if (mergeInst == nullptr || mergeInst->NumOperands() != 3 ||
                            mergeInst->GetSingleWordOperand(2) !=
                                static_cast<uint32_t>(spv::LoopControlMask::MaskNone)) {
                            break;
                        }
                        mergeInst->SetOperand(
                            2, {static_cast<uint32_t>(spv::LoopControlMask::Unroll)});
                        nestIterations *= iterations;
                        modified = true;
                    }
                }

                if (!modified) {
                    return Status::SuccessWithoutChange;
                }
                MGLOG_D("[spirv] resource array index: marked enclosing loops for full unrolling");
                return Status::SuccessWithChange;
            }

            spvtools::opt::Pass::Status LegalizeResourceArrayIndexPass::LowerToConstantSwitch() {
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
                        const LoweringOutcome outcome =
                            LowerOneChain(use.accessChain, use.arrayLength, use.isImageArray);
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

            LegalizeResourceArrayIndexPass::LoweringOutcome
            LegalizeResourceArrayIndexPass::LowerOneChain(Instruction* accessChain, uint32_t arrayLength,
                                                          bool isImageArray) {
                auto* irContext = context();
                if (arrayLength == 0 || arrayLength > kMaxLoweredArrayLength) {
                    MGLOG_D("[spirv] resource array index: array length %u is not lowerable",
                            arrayLength);
                    return LoweringOutcome::Declined;
                }
                if (!IsLowerableIndexType(irContext, accessChain->GetSingleWordInOperand(1))) {
                    return LoweringOutcome::Declined;
                }
                if (isImageArray) {
                    return LowerImageChain(accessChain, arrayLength);
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
            LegalizeResourceArrayIndexPass::LoweringOutcome
            LegalizeResourceArrayIndexPass::LowerStore(Instruction* accessChain, uint32_t arrayLength,
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
            LegalizeResourceArrayIndexPass::LoweringOutcome
            LegalizeResourceArrayIndexPass::LowerLoad(Instruction* accessChain, uint32_t arrayLength,
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

            // An image array's chain is never stored or loaded THROUGH the way a storage
            // block's is: it is OpLoad-ed once into an opaque image object, and the image ops
            // consume that object. So this resolves the chain one CONSUMER at a time - the
            // round loop in LowerToConstantSwitch recollects after each - and refuses anything
            // that is not a plain read or write of the loaded image.
            LegalizeResourceArrayIndexPass::LoweringOutcome
            LegalizeResourceArrayIndexPass::LowerImageChain(Instruction* accessChain, uint32_t arrayLength) {
                auto* irContext = context();

                std::vector<Instruction*> loads;
                bool unsupportedUse = false;
                irContext->get_def_use_mgr()->ForEachUser(accessChain, [&](Instruction* user) {
                    switch (user->opcode()) {
                    case spv::Op::OpName:
                    case spv::Op::OpDecorate:
                    case spv::Op::OpDecorateId:
                        return;
                    case spv::Op::OpLoad:
                        // Memory operands would be dropped by the per-element rebuild, so a
                        // load carrying any is refused instead.
                        if (user->NumInOperands() == 1) {
                            loads.push_back(user);
                        } else {
                            unsupportedUse = true;
                        }
                        return;
                    default:
                        // OpImageTexelPointer above all: that is how an imageAtomic* reaches
                        // the array, and running one per element would perform every OTHER
                        // element's atomic as well - a read can be thrown away, a
                        // read-modify-write cannot.
                        unsupportedUse = true;
                        return;
                    }
                });

                if (unsupportedUse) {
                    MGLOG_D("[spirv] image array index: chain %%%u has a use this pass cannot rewrite",
                            accessChain->result_id());
                    return LoweringOutcome::Declined;
                }

                if (loads.empty()) {
                    // No uses left: the chain itself is what detection is still seeing.
                    irContext->KillInst(accessChain);
                    irContext->InvalidateAnalysesExceptFor(IRContext::kAnalysisNone);
                    return LoweringOutcome::Changed;
                }

                Instruction* load = loads.front();
                Instruction* consumer = nullptr;
                bool unsupportedConsumer = false;
                irContext->get_def_use_mgr()->ForEachUser(load, [&](Instruction* user) {
                    switch (user->opcode()) {
                    case spv::Op::OpName:
                    case spv::Op::OpDecorate:
                    case spv::Op::OpDecorateId:
                        return;
                    case spv::Op::OpImageWrite:
                    case spv::Op::OpImageRead:
                    // imageSize()/imageSamples() carry the image in the same leading operand
                    // position as an OpImageRead and produce an int or int vector, so the same
                    // select ladder rebuilds them exactly - and a size query touches no memory
                    // at all, which makes evaluating it for every element strictly safer than
                    // the read the ladder was written for.
                    case spv::Op::OpImageQuerySize:
                    case spv::Op::OpImageQuerySizeLod:
                        if (consumer == nullptr) consumer = user;
                        return;
                    default:
                        // A sampled-image construction, a copy, an argument to a function:
                        // shapes whose per-element rebuild this pass cannot spell exactly.
                        unsupportedConsumer = true;
                        return;
                    }
                });

                if (unsupportedConsumer) {
                    MGLOG_D("[spirv] image array index: the image loaded from chain %%%u is consumed by "
                            "an operation this pass cannot rewrite",
                            accessChain->result_id());
                    return LoweringOutcome::Declined;
                }
                if (consumer == nullptr) {
                    irContext->KillInst(load);
                    irContext->InvalidateAnalysesExceptFor(IRContext::kAnalysisNone);
                    return LoweringOutcome::Changed;
                }

                return consumer->opcode() == spv::Op::OpImageWrite
                           ? LowerImageWrite(accessChain, arrayLength, load, consumer)
                           : LowerImageReadOrQuery(accessChain, arrayLength, load, consumer);
            }

            // Drops |load| and |accessChain| once the rewrite above has taken their last user,
            // in that order - the load is what uses the chain. Anything still using either is
            // another consumer a later round will come back for.
            void LegalizeResourceArrayIndexPass::KillImageChainIfDead(Instruction* accessChain,
                                                                      Instruction* load) {
                auto* irContext = context();
                if (irContext->get_def_use_mgr()->NumUsers(load) == 0) {
                    irContext->KillInst(load);
                }
                if (irContext->get_def_use_mgr()->NumUsers(accessChain) == 0) {
                    irContext->KillInst(accessChain);
                }
            }

            // switch (idx) { case 0: imageStore(arr[0], ...); break; case 1: ... }
            //
            // The same block split as LowerStore, for the same reason: whatever followed the
            // write still runs exactly once on every path, and an index outside [0, length)
            // reaches the default target - the merge block - so nothing is written, which is
            // what indexing an image array out of range already meant.
            LegalizeResourceArrayIndexPass::LoweringOutcome
            LegalizeResourceArrayIndexPass::LowerImageWrite(Instruction* accessChain, uint32_t arrayLength,
                                                            Instruction* load, Instruction* imageWrite) {
                auto* irContext = context();
                BasicBlock* block = irContext->get_instr_block(imageWrite);
                if (block == nullptr) {
                    return LoweringOutcome::Declined;
                }
                // Splitting a loop header keeps the label - and so the back edge's target - on
                // the first half while the OpLoopMerge moves to the second, which is not a loop
                // any more. Refuse instead of producing that.
                if (block->GetLoopMergeInst() != nullptr) {
                    MGLOG_D("[spirv] image array index: write sits in a loop header, declining");
                    return LoweringOutcome::Declined;
                }
                Function* function = block->GetParent();
                if (function == nullptr) {
                    return LoweringOutcome::Declined;
                }

                const uint32_t indexId = accessChain->GetSingleWordInOperand(1);
                const uint32_t imageTypeId = load->type_id();
                // Coordinate, texel and any image operands, verbatim: only the image itself is
                // per-element.
                std::vector<Operand> tailOperands;
                for (uint32_t i = 1; i < imageWrite->NumInOperands(); ++i) {
                    tailOperands.push_back(imageWrite->GetInOperand(i));
                }

                const uint32_t mergeLabelId = irContext->TakeNextId();
                block->SplitBasicBlock(irContext, mergeLabelId, BasicBlock::iterator(imageWrite));
                // |imageWrite| now heads the merge block; the per-element writes replace it.
                irContext->KillInst(imageWrite);

                std::vector<std::pair<Operand::OperandData, uint32_t>> targets;
                targets.reserve(arrayLength);
                BasicBlock* insertAfter = block;
                for (uint32_t element = 0; element < arrayLength; ++element) {
                    const uint32_t caseLabelId = irContext->TakeNextId();
                    auto caseBlock = MakeUnique<BasicBlock>(MakeUnique<Instruction>(
                        irContext, spv::Op::OpLabel, 0, caseLabelId, std::initializer_list<Operand>{}));
                    caseBlock->SetParent(function);
                    BasicBlock* casePtr = function->InsertBasicBlockAfter(std::move(caseBlock), insertAfter);
                    // Hand-built label; see LowerStore for why it has to be registered here.
                    irContext->AnalyzeDefUse(casePtr->GetLabelInst());
                    irContext->set_instr_block(casePtr->GetLabelInst(), casePtr);

                    InstructionBuilder caseBuilder(
                        irContext, casePtr,
                        IRContext::kAnalysisDefUse | IRContext::kAnalysisInstrToBlockMapping);
                    const uint32_t constantId = ConstantLikeIndex(irContext, indexId, element);
                    Instruction* elementChain =
                        CloneChainWithConstantIndex(caseBuilder, irContext, accessChain, constantId);
                    Instruction* elementImage =
                        caseBuilder.AddLoad(imageTypeId, elementChain->result_id());

                    std::vector<Operand> writeOperands;
                    writeOperands.push_back({SPV_OPERAND_TYPE_ID, {elementImage->result_id()}});
                    for (const Operand& tailOperand : tailOperands) {
                        writeOperands.push_back(tailOperand);
                    }
                    caseBuilder.AddInstruction(
                        MakeUnique<Instruction>(irContext, spv::Op::OpImageWrite, 0, 0, writeOperands));
                    caseBuilder.AddBranch(mergeLabelId);

                    targets.push_back({Operand::OperandData{element}, caseLabelId});
                    insertAfter = casePtr;
                }

                InstructionBuilder switchBuilder(
                    irContext, block, IRContext::kAnalysisDefUse | IRContext::kAnalysisInstrToBlockMapping);
                switchBuilder.AddSwitch(indexId, mergeLabelId, targets, mergeLabelId);

                KillImageChainIfDead(accessChain, load);
                irContext->InvalidateAnalysesExceptFor(IRContext::kAnalysisNone);
                MGLOG_D("[spirv] image array index: lowered a dynamic imageStore to a %u-way switch",
                        arrayLength);
                return LoweringOutcome::Changed;
            }

            // A value-producing consumer - an OpImageRead, or an imageSize()/imageSamples() query -
            // needs no control flow: run it against every element through a constant index and pick
            // with OpSelect. The selection happens on the RESULT, not on the image object - an
            // opaque type may not be selected at all (pre-1.4 OpSelect takes pointers, scalars
            // and vectors only, and ESSL has no ternary on an image), so what is duplicated is
            // the consuming instruction itself. Every such consumer carries the image in in-operand
            // 0 and nothing else that is per-element, so one rebuild spells all of them.
            //
            // Running the elements the shader did not ask for is safe: every one of them is an
            // image this stage already declares, and GL 4.6 7.11.2 makes a load through an
            // image unit whose binding is missing or incompatible return undefined DATA - never
            // an error, and never a fault - which the select then discards. A size query does not
            // even touch memory. Contrast an imageAtomic*, which LowerImageChain refuses for
            // exactly the opposite reason.
            LegalizeResourceArrayIndexPass::LoweringOutcome
            LegalizeResourceArrayIndexPass::LowerImageReadOrQuery(Instruction* accessChain,
                                                                  uint32_t arrayLength, Instruction* load,
                                                                  Instruction* consumer) {
                auto* irContext = context();
                uint32_t conditionTypeId = 0;
                uint32_t dimension = 0;
                if (!TryGetSelectConditionType(irContext, consumer->type_id(), &conditionTypeId,
                                               &dimension)) {
                    MGLOG_D("[spirv] image array index: result type is not selectable, declining");
                    return LoweringOutcome::Declined;
                }
                const uint32_t boolTypeId = irContext->get_type_mgr()->GetBoolTypeId();
                const uint32_t indexId = accessChain->GetSingleWordInOperand(1);
                const uint32_t imageTypeId = load->type_id();
                std::vector<Operand> tailOperands;
                for (uint32_t i = 1; i < consumer->NumInOperands(); ++i) {
                    tailOperands.push_back(consumer->GetInOperand(i));
                }

                InstructionBuilder builder(
                    irContext, consumer,
                    IRContext::kAnalysisDefUse | IRContext::kAnalysisInstrToBlockMapping);

                uint32_t selectedId = 0;
                for (uint32_t element = 0; element < arrayLength; ++element) {
                    const uint32_t constantId = ConstantLikeIndex(irContext, indexId, element);
                    Instruction* elementChain =
                        CloneChainWithConstantIndex(builder, irContext, accessChain, constantId);
                    Instruction* elementImage = builder.AddLoad(imageTypeId, elementChain->result_id());

                    std::vector<Operand> elementOperands;
                    elementOperands.push_back({SPV_OPERAND_TYPE_ID, {elementImage->result_id()}});
                    for (const Operand& tailOperand : tailOperands) {
                        elementOperands.push_back(tailOperand);
                    }
                    Instruction* elementResult = builder.AddInstruction(
                        MakeUnique<Instruction>(irContext, consumer->opcode(), consumer->type_id(),
                                                irContext->TakeNextId(), elementOperands));
                    if (element == 0) {
                        // Element 0 is the else-arm of the whole ladder, so an out-of-range
                        // index reads it - an undefined element for an undefined index.
                        selectedId = elementResult->result_id();
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
                                     .AddSelect(consumer->type_id(), conditionId,
                                                elementResult->result_id(), selectedId)
                                     ->result_id();
                }

                irContext->ReplaceAllUsesWith(consumer->result_id(), selectedId);
                irContext->KillInst(consumer);
                KillImageChainIfDead(accessChain, load);
                irContext->InvalidateAnalysesExceptFor(IRContext::kAnalysisNone);
                MGLOG_D("[spirv] image array index: lowered a dynamic image read/query to %u "
                        "constant-indexed operations",
                        arrayLength);
                return LoweringOutcome::Changed;
            }

            spvtools::Optimizer::PassToken
            LegalizeResourceArrayIndexPass::CreateMarkLoopsForUnrollPass() {
                return spvtools::Optimizer::PassToken(
                    MakeUnique<LegalizeResourceArrayIndexPass>(Mode::MarkLoopsForUnroll));
            }

            spvtools::Optimizer::PassToken
            LegalizeResourceArrayIndexPass::CreateLowerToConstantSwitchPass() {
                return spvtools::Optimizer::PassToken(
                    MakeUnique<LegalizeResourceArrayIndexPass>(Mode::LowerToConstantSwitch));
            }
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
