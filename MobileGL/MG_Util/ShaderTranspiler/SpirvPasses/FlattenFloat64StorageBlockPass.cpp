// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/FlattenFloat64StorageBlockPass.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "FlattenFloat64StorageBlockPass.h"

#include "spirv.hpp"
#include "source/opt/basic_block.h"
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

#include <algorithm>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace MobileGL {
    namespace MG_Util {
        namespace ShaderTranspiler {
            namespace {
                using spvtools::MakeUnique;
                using spvtools::opt::Instruction;
                using spvtools::opt::InstructionBuilder;
                using spvtools::opt::IRContext;
                using spvtools::opt::Module;
                using spvtools::opt::Operand;

                // The flattened array's element, and the granularity every offset and stride in
                // the block has to land on.
                constexpr uint32_t kWordBytes = 4u;
                // A block wider than any GL implementation lets one binding cover is refused
                // rather than expanded into an array nothing could address. GL 4.6 core table
                // 23.64 puts the minimum GL_MAX_SHADER_STORAGE_BLOCK_SIZE at 2^24 bytes; this is
                // a generous multiple of that and exists only to bound the rewrite.
                constexpr uint64_t kMaxBlockBytes = 1ull << 27;
                // The most scalars one load or store may decompose into. A whole-aggregate copy
                // becomes one word access per scalar, so without a cap a `dvec4 data[4096]`
                // member would turn a two-instruction copy into a 32k-instruction one.
                constexpr uint32_t kMaxLeavesPerAccess = 1024u;
                // The analyses every builder in this pass keeps current as it inserts.
                constexpr IRContext::Analysis kPreservedAnalyses = static_cast<IRContext::Analysis>(
                    static_cast<uint32_t>(IRContext::kAnalysisDefUse) |
                    static_cast<uint32_t>(IRContext::kAnalysisInstrToBlockMapping));

                // How a type sits in memory, as the ENCLOSING struct member described it.
                // MatrixStride and RowMajor are member decorations rather than type decorations,
                // so a matrix type carries no layout of its own and the walk has to hand it down -
                // through arrays of matrices too, which is why this rides alongside the type id
                // instead of being looked up from it.
                struct TypeCursor {
                    uint32_t typeId = 0;
                    uint32_t matrixStride = 0;
                    bool rowMajor = false;
                };

                // One access chain rooted at a flattened block's variable, and everything the
                // rewrite needs so it does not have to walk the type tree a second time.
                struct ChainPlan {
                    Instruction* chain = nullptr;
                    uint32_t variableId = 0;
                    // What the chain's CONSTANT indices contribute, in words.
                    uint32_t constantWords = 0;
                    // Its non-constant indices, as (index value id, words per step).
                    std::vector<std::pair<uint32_t, uint32_t>> dynamicTerms;
                    TypeCursor pointee;
                    std::vector<Instruction*> loads;
                    std::vector<Instruction*> stores;
                };

                struct BlockPlan {
                    Instruction* structType = nullptr;
                    uint32_t storageClass = 0;
                    // A bounded block's length in words. For an open-ended block - one whose
                    // last member is a runtime array - the FIXED PREFIX in words, i.e. the
                    // runtime array's own offset, which is where its element 0 starts.
                    uint32_t wordCount = 0;
                    bool openEnded = false;
                    // The original runtime array's stride in words; what one element of it
                    // steps by, and what its word count divides by to become a length.
                    uint32_t tailStrideWords = 0;
                    std::vector<ChainPlan> chains;
                    // The OpArrayLength users of an open-ended block's variables, which count
                    // WORDS once the member is a `uint[]` and so have to be rewritten too.
                    std::vector<Instruction*> arrayLengths;
                };

                bool IsDoubleType(const Instruction* type) {
                    return type != nullptr && type->opcode() == spv::Op::OpTypeFloat &&
                           type->NumInOperands() >= 1 && type->GetSingleWordInOperand(0) == 64u;
                }

                // Byte size of a scalar this pass can carry, or 0 for one it cannot.
                uint32_t ScalarByteSize(const Instruction* type) {
                    if (type == nullptr) return 0;
                    if (type->opcode() != spv::Op::OpTypeFloat && type->opcode() != spv::Op::OpTypeInt) {
                        return 0;
                    }
                    const uint32_t width = type->GetSingleWordInOperand(0);
                    if (width == 32u) return 4u;
                    if (width == 64u && type->opcode() == spv::Op::OpTypeFloat) return 8u;
                    return 0;
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

                bool HasMemberDecoration(IRContext* context, uint32_t structId, uint32_t member,
                                         spv::Decoration kind) {
                    for (Instruction* decoration :
                         context->get_decoration_mgr()->GetDecorationsFor(structId, false)) {
                        if (decoration->opcode() != spv::Op::OpMemberDecorate ||
                            decoration->NumInOperands() < 3 ||
                            decoration->GetSingleWordInOperand(1) != member) {
                            continue;
                        }
                        if (static_cast<spv::Decoration>(decoration->GetSingleWordInOperand(2)) == kind) {
                            return true;
                        }
                    }
                    return false;
                }

                bool HasDecoration(IRContext* context, uint32_t id, spv::Decoration kind) {
                    for (Instruction* decoration : context->get_decoration_mgr()->GetDecorationsFor(id, false)) {
                        if (decoration->opcode() != spv::Op::OpDecorate || decoration->NumInOperands() < 2) {
                            continue;
                        }
                        if (static_cast<spv::Decoration>(decoration->GetSingleWordInOperand(1)) == kind) {
                            return true;
                        }
                    }
                    return false;
                }

                // The cursor for member `member` of a struct: its type, plus the matrix layout
                // that member's own decorations describe.
                TypeCursor MemberCursor(IRContext* context, const Instruction* structType, uint32_t member) {
                    TypeCursor cursor;
                    cursor.typeId = structType->GetSingleWordInOperand(member);
                    uint32_t stride = 0;
                    if (TryGetMemberDecorationLiteral(context, structType->result_id(), member,
                                                      spv::Decoration::MatrixStride, &stride)) {
                        cursor.matrixStride = stride;
                    }
                    cursor.rowMajor = HasMemberDecoration(context, structType->result_id(), member,
                                                          spv::Decoration::RowMajor);
                    return cursor;
                }

                // Byte size of a type as it is laid out INSIDE a block, or 0 when this pass
                // cannot describe it (a runtime array - the one place a block may have one is
                // its last member, which MeasureBlock handles above this - a width it does not
                // carry, a matrix with no stride or a row-major one).
                uint32_t LaidOutByteSize(IRContext* context, const TypeCursor& cursor) {
                    const Instruction* type = context->get_def_use_mgr()->GetDef(cursor.typeId);
                    if (type == nullptr) return 0;
                    switch (type->opcode()) {
                    case spv::Op::OpTypeInt:
                    case spv::Op::OpTypeFloat:
                        return ScalarByteSize(type);
                    case spv::Op::OpTypeVector: {
                        const uint32_t component = ScalarByteSize(
                            context->get_def_use_mgr()->GetDef(type->GetSingleWordInOperand(0)));
                        if (component == 0) return 0;
                        return component * type->GetSingleWordInOperand(1);
                    }
                    case spv::Op::OpTypeMatrix: {
                        if (cursor.matrixStride == 0 || cursor.rowMajor) return 0;
                        return cursor.matrixStride * type->GetSingleWordInOperand(1);
                    }
                    case spv::Op::OpTypeArray: {
                        uint32_t stride = 0;
                        if (!TryGetDecorationLiteral(context, cursor.typeId, spv::Decoration::ArrayStride,
                                                     &stride) ||
                            stride == 0) {
                            return 0;
                        }
                        const spvtools::opt::analysis::Constant* length =
                            context->get_constant_mgr()->FindDeclaredConstant(type->GetSingleWordInOperand(1));
                        if (length == nullptr || length->AsIntConstant() == nullptr) return 0;
                        const uint64_t total = static_cast<uint64_t>(stride) *
                                               static_cast<uint64_t>(length->AsIntConstant()->GetU32BitValue());
                        return total > kMaxBlockBytes ? 0u : static_cast<uint32_t>(total);
                    }
                    case spv::Op::OpTypeStruct: {
                        uint64_t end = 0;
                        for (uint32_t member = 0; member < type->NumInOperands(); ++member) {
                            uint32_t offset = 0;
                            if (!TryGetMemberDecorationLiteral(context, cursor.typeId, member,
                                                               spv::Decoration::Offset, &offset)) {
                                return 0;
                            }
                            const uint32_t size = LaidOutByteSize(context, MemberCursor(context, type, member));
                            if (size == 0) return 0;
                            end = std::max<uint64_t>(end, static_cast<uint64_t>(offset) + size);
                        }
                        return end > kMaxBlockBytes ? 0u : static_cast<uint32_t>(end);
                    }
                    default:
                        return 0;
                    }
                }

                // Whether this type decomposes into scalars the rewrite can move one word at a
                // time. |leafCount| counts them, so a whole-aggregate access can be refused
                // before it is expanded; passing NULL asks the SHAPE question alone - is this
                // type addressable at all - and then identical array elements and vector
                // components are walked once instead of once each, because the answer cannot
                // differ between them and the walk of a big one would not be free.
                //
                // The two questions are separate because only a LOAD or a STORE expands into
                // leaves, and the cap bounds one of those. How large a runtime array's element
                // is says nothing about how many scalars a single access to it moves, so
                // MeasureBlock asks for the shape and BuildPlans applies the cap where it
                // belongs - per chain, to the type that chain actually names.
                bool CanDecompose(IRContext* context, const TypeCursor& cursor, uint32_t* leafCount) {
                    const Instruction* type = context->get_def_use_mgr()->GetDef(cursor.typeId);
                    if (type == nullptr) return false;
                    switch (type->opcode()) {
                    case spv::Op::OpTypeInt:
                    case spv::Op::OpTypeFloat:
                        if (ScalarByteSize(type) == 0) return false;
                        if (leafCount == nullptr) return true;
                        ++*leafCount;
                        return *leafCount <= kMaxLeavesPerAccess;
                    case spv::Op::OpTypeVector: {
                        TypeCursor component;
                        component.typeId = type->GetSingleWordInOperand(0);
                        const uint32_t repeats =
                            leafCount == nullptr ? 1u : type->GetSingleWordInOperand(1);
                        for (uint32_t i = 0; i < repeats; ++i) {
                            if (!CanDecompose(context, component, leafCount)) return false;
                        }
                        return true;
                    }
                    case spv::Op::OpTypeMatrix: {
                        if (cursor.matrixStride == 0 || cursor.rowMajor ||
                            cursor.matrixStride % kWordBytes != 0) {
                            return false;
                        }
                        TypeCursor column;
                        column.typeId = type->GetSingleWordInOperand(0);
                        const uint32_t repeats =
                            leafCount == nullptr ? 1u : type->GetSingleWordInOperand(1);
                        for (uint32_t i = 0; i < repeats; ++i) {
                            if (!CanDecompose(context, column, leafCount)) return false;
                        }
                        return true;
                    }
                    case spv::Op::OpTypeArray: {
                        uint32_t stride = 0;
                        if (!TryGetDecorationLiteral(context, cursor.typeId, spv::Decoration::ArrayStride,
                                                     &stride) ||
                            stride == 0 || stride % kWordBytes != 0) {
                            return false;
                        }
                        const spvtools::opt::analysis::Constant* length =
                            context->get_constant_mgr()->FindDeclaredConstant(type->GetSingleWordInOperand(1));
                        if (length == nullptr || length->AsIntConstant() == nullptr) return false;
                        const uint32_t count = length->AsIntConstant()->GetU32BitValue();
                        if (count == 0) return false;
                        if (leafCount != nullptr && count > kMaxLeavesPerAccess) return false;
                        TypeCursor element = cursor;
                        element.typeId = type->GetSingleWordInOperand(0);
                        const uint32_t repeats = leafCount == nullptr ? 1u : count;
                        for (uint32_t i = 0; i < repeats; ++i) {
                            if (!CanDecompose(context, element, leafCount)) return false;
                        }
                        return true;
                    }
                    case spv::Op::OpTypeStruct: {
                        for (uint32_t member = 0; member < type->NumInOperands(); ++member) {
                            uint32_t offset = 0;
                            if (!TryGetMemberDecorationLiteral(context, cursor.typeId, member,
                                                               spv::Decoration::Offset, &offset) ||
                                offset % kWordBytes != 0) {
                                return false;
                            }
                            if (!CanDecompose(context, MemberCursor(context, type, member), leafCount)) {
                                return false;
                            }
                        }
                        return true;
                    }
                    default:
                        return false;
                    }
                }

                // Measures the block struct itself. A bounded block reports its laid-out byte
                // size; a block whose LAST member is a runtime array - the only place GLSL lets
                // one stand, and the only place SPIR-V lets a Block have one - reports the byte
                // offset that array starts at and says so through |openEnded|, with the array's
                // stride alongside. A runtime array anywhere else, one without a stride the
                // words can step by, or one whose element the rewrite could not take apart is a
                // shape this pass does not describe, and so is a bounded block it cannot size.
                bool MeasureBlock(IRContext* context, const Instruction* structType, uint32_t* bytes,
                                  bool* openEnded, uint32_t* tailStrideBytes) {
                    *bytes = 0;
                    *openEnded = false;
                    *tailStrideBytes = 0;
                    const uint32_t structId = structType->result_id();
                    const uint32_t memberCount = structType->NumInOperands();
                    uint64_t end = 0;
                    for (uint32_t member = 0; member < memberCount; ++member) {
                        uint32_t offset = 0;
                        if (!TryGetMemberDecorationLiteral(context, structId, member, spv::Decoration::Offset,
                                                           &offset)) {
                            return false;
                        }
                        const TypeCursor cursor = MemberCursor(context, structType, member);
                        const Instruction* type = context->get_def_use_mgr()->GetDef(cursor.typeId);
                        if (type == nullptr) return false;
                        if (type->opcode() == spv::Op::OpTypeRuntimeArray) {
                            if (member + 1 != memberCount) return false;
                            uint32_t stride = 0;
                            if (!TryGetDecorationLiteral(context, cursor.typeId, spv::Decoration::ArrayStride,
                                                         &stride) ||
                                stride == 0 || stride % kWordBytes != 0) {
                                return false;
                            }
                            // The member's own matrix decorations describe the array's ELEMENTS,
                            // exactly as they do for a bounded array of matrices. Only the shape
                            // is asked for: how big one element is decides nothing about how
                            // many scalars one access moves, and a leaf cap here would decline a
                            // block over a member the shader may never read whole.
                            TypeCursor element = cursor;
                            element.typeId = type->GetSingleWordInOperand(0);
                            if (!CanDecompose(context, element, nullptr)) return false;
                            // Element 0 has to start past every fixed member, or the words the
                            // prefix owns and the words the array owns would overlap.
                            if (offset < end) return false;
                            end = offset;
                            *openEnded = true;
                            *tailStrideBytes = stride;
                            break;
                        }
                        const uint32_t size = LaidOutByteSize(context, cursor);
                        if (size == 0) return false;
                        end = std::max<uint64_t>(end, static_cast<uint64_t>(offset) + size);
                    }
                    if (end > kMaxBlockBytes) return false;
                    *bytes = static_cast<uint32_t>(end);
                    return true;
                }

                bool TypeContainsFloat64(IRContext* context, uint32_t typeId,
                                         std::unordered_set<uint32_t>& visiting) {
                    const Instruction* type = context->get_def_use_mgr()->GetDef(typeId);
                    if (type == nullptr || !visiting.insert(typeId).second) return false;
                    switch (type->opcode()) {
                    case spv::Op::OpTypeFloat:
                        return type->GetSingleWordInOperand(0) == 64u;
                    case spv::Op::OpTypeVector:
                    case spv::Op::OpTypeMatrix:
                    case spv::Op::OpTypeArray:
                    case spv::Op::OpTypeRuntimeArray:
                        return TypeContainsFloat64(context, type->GetSingleWordInOperand(0), visiting);
                    case spv::Op::OpTypeStruct:
                        for (uint32_t member = 0; member < type->NumInOperands(); ++member) {
                            if (TypeContainsFloat64(context, type->GetSingleWordInOperand(member), visiting)) {
                                return true;
                            }
                        }
                        return false;
                    default:
                        return false;
                    }
                }

                // The constant an index operand names, or false when it is not one.
                bool TryGetConstantIndex(IRContext* context, uint32_t id, uint32_t* value) {
                    const spvtools::opt::analysis::Constant* constant =
                        context->get_constant_mgr()->FindDeclaredConstant(id);
                    if (constant == nullptr || constant->AsIntConstant() == nullptr) return false;
                    *value = constant->AsIntConstant()->GetU32BitValue();
                    return true;
                }

                // Walks one access chain against the block's type tree, recording the byte offset
                // it names as a constant part plus a list of (index, stride) terms. False for any
                // shape the rewrite cannot address exactly.
                bool PlanChain(IRContext* context, Instruction* chain, const TypeCursor& blockCursor,
                               ChainPlan* plan) {
                    TypeCursor cursor = blockCursor;
                    uint64_t constantBytes = 0;
                    for (uint32_t operand = 1; operand < chain->NumInOperands(); ++operand) {
                        const uint32_t indexId = chain->GetSingleWordInOperand(operand);
                        const Instruction* type = context->get_def_use_mgr()->GetDef(cursor.typeId);
                        if (type == nullptr) return false;

                        uint32_t stride = 0;
                        if (type->opcode() == spv::Op::OpTypeStruct) {
                            uint32_t member = 0;
                            if (!TryGetConstantIndex(context, indexId, &member) ||
                                member >= type->NumInOperands()) {
                                return false;
                            }
                            uint32_t offset = 0;
                            if (!TryGetMemberDecorationLiteral(context, cursor.typeId, member,
                                                               spv::Decoration::Offset, &offset) ||
                                offset % kWordBytes != 0) {
                                return false;
                            }
                            constantBytes += offset;
                            cursor = MemberCursor(context, type, member);
                            if (constantBytes > kMaxBlockBytes) return false;
                            continue;
                        }

                        switch (type->opcode()) {
                        case spv::Op::OpTypeArray:
                        // A runtime array steps exactly like a bounded one; only its end is
                        // unknown, and a chain never needs that.
                        case spv::Op::OpTypeRuntimeArray:
                            if (!TryGetDecorationLiteral(context, cursor.typeId, spv::Decoration::ArrayStride,
                                                         &stride)) {
                                return false;
                            }
                            cursor.typeId = type->GetSingleWordInOperand(0);
                            break;
                        case spv::Op::OpTypeMatrix:
                            if (cursor.rowMajor || cursor.matrixStride == 0) return false;
                            stride = cursor.matrixStride;
                            cursor.typeId = type->GetSingleWordInOperand(0);
                            cursor.matrixStride = 0;
                            break;
                        case spv::Op::OpTypeVector:
                            stride = ScalarByteSize(
                                context->get_def_use_mgr()->GetDef(type->GetSingleWordInOperand(0)));
                            cursor.typeId = type->GetSingleWordInOperand(0);
                            cursor.matrixStride = 0;
                            break;
                        default:
                            return false;
                        }

                        if (stride == 0 || stride % kWordBytes != 0) return false;
                        uint32_t index = 0;
                        if (TryGetConstantIndex(context, indexId, &index)) {
                            constantBytes += static_cast<uint64_t>(index) * stride;
                            if (constantBytes > kMaxBlockBytes) return false;
                        } else {
                            plan->dynamicTerms.emplace_back(indexId, stride / kWordBytes);
                        }
                    }

                    if (constantBytes % kWordBytes != 0) return false;
                    plan->constantWords = static_cast<uint32_t>(constantBytes / kWordBytes);
                    plan->pointee = cursor;
                    return true;
                }

                // Everything the rewrite emits, over one module's shared scalar types.
                class Emitter {
                public:
                    // Where one block's words live: the variable holding them, the pointer type
                    // that reaches one, and the base index the chain resolved to.
                    struct Access {
                        uint32_t variableId = 0;
                        uint32_t wordPointerTypeId = 0;
                        uint32_t baseWordId = 0;
                        uint32_t memberZeroId = 0;
                    };

                    Emitter(IRContext* context, uint32_t uintTypeId, uint32_t boolTypeId, uint32_t floatTypeId)
                        : m_context(context), m_uintTypeId(uintTypeId), m_boolTypeId(boolTypeId),
                          m_floatTypeId(floatTypeId) {}

                    uint32_t UintConstant(uint32_t value) {
                        const spvtools::opt::analysis::Type* type =
                            m_context->get_type_mgr()->GetType(m_uintTypeId);
                        const spvtools::opt::analysis::Constant* constant =
                            m_context->get_constant_mgr()->GetConstant(type, {value});
                        return m_context->get_constant_mgr()->GetDefiningInstruction(constant)->result_id();
                    }

                    // The word index the chain names, materialised at the chain's own position so
                    // every load and store that uses it is dominated by it.
                    uint32_t WordIndexOf(const ChainPlan& plan) {
                        InstructionBuilder builder(m_context, plan.chain, kPreservedAnalyses);
                        uint32_t total = 0;
                        for (const auto& [indexId, wordsPerStep] : plan.dynamicTerms) {
                            uint32_t term = AsUint(builder, indexId);
                            if (wordsPerStep != 1u) {
                                term = Binary(builder, spv::Op::OpIMul, m_uintTypeId, term,
                                              UintConstant(wordsPerStep));
                            }
                            total = total == 0 ? term
                                               : Binary(builder, spv::Op::OpIAdd, m_uintTypeId, total, term);
                        }
                        if (total == 0) return UintConstant(plan.constantWords);
                        if (plan.constantWords == 0) return total;
                        return Binary(builder, spv::Op::OpIAdd, m_uintTypeId, total,
                                      UintConstant(plan.constantWords));
                    }

                    // Rebuilds the value an OpLoad of `cursor` would have produced, out of the
                    // words that live at `baseWordId + relWords`.
                    uint32_t BuildValue(InstructionBuilder& builder, const Access& access,
                                        const TypeCursor& cursor, uint32_t relWords) {
                        const Instruction* type = m_context->get_def_use_mgr()->GetDef(cursor.typeId);
                        switch (type->opcode()) {
                        case spv::Op::OpTypeInt:
                        case spv::Op::OpTypeFloat: {
                            if (IsDoubleType(type)) {
                                const uint32_t lo = LoadWord(builder, access, relWords);
                                const uint32_t hi = LoadWord(builder, access, relWords + 1);
                                const uint32_t narrowed = builder
                                                              .AddUnaryOp(m_floatTypeId, spv::Op::OpBitcast,
                                                                          NarrowDoubleBits(builder, lo, hi))
                                                              ->result_id();
                                return builder
                                    .AddUnaryOp(cursor.typeId, spv::Op::OpFConvert, narrowed)
                                    ->result_id();
                            }
                            const uint32_t word = LoadWord(builder, access, relWords);
                            if (cursor.typeId == m_uintTypeId) return word;
                            return builder.AddUnaryOp(cursor.typeId, spv::Op::OpBitcast, word)->result_id();
                        }
                        case spv::Op::OpTypeVector: {
                            TypeCursor component;
                            component.typeId = type->GetSingleWordInOperand(0);
                            const uint32_t step = ComponentWords(component.typeId);
                            std::vector<uint32_t> parts;
                            for (uint32_t i = 0; i < type->GetSingleWordInOperand(1); ++i) {
                                parts.push_back(BuildValue(builder, access, component, relWords + i * step));
                            }
                            return builder.AddCompositeConstruct(cursor.typeId, parts)->result_id();
                        }
                        case spv::Op::OpTypeMatrix: {
                            TypeCursor column;
                            column.typeId = type->GetSingleWordInOperand(0);
                            const uint32_t step = cursor.matrixStride / kWordBytes;
                            std::vector<uint32_t> parts;
                            for (uint32_t i = 0; i < type->GetSingleWordInOperand(1); ++i) {
                                parts.push_back(BuildValue(builder, access, column, relWords + i * step));
                            }
                            return builder.AddCompositeConstruct(cursor.typeId, parts)->result_id();
                        }
                        case spv::Op::OpTypeArray: {
                            TypeCursor element = cursor;
                            element.typeId = type->GetSingleWordInOperand(0);
                            uint32_t stride = 0;
                            TryGetDecorationLiteral(m_context, cursor.typeId, spv::Decoration::ArrayStride,
                                                    &stride);
                            const uint32_t count = m_context->get_constant_mgr()
                                                       ->FindDeclaredConstant(type->GetSingleWordInOperand(1))
                                                       ->AsIntConstant()
                                                       ->GetU32BitValue();
                            std::vector<uint32_t> parts;
                            for (uint32_t i = 0; i < count; ++i) {
                                parts.push_back(
                                    BuildValue(builder, access, element, relWords + i * (stride / kWordBytes)));
                            }
                            return builder.AddCompositeConstruct(cursor.typeId, parts)->result_id();
                        }
                        case spv::Op::OpTypeStruct: {
                            std::vector<uint32_t> parts;
                            for (uint32_t member = 0; member < type->NumInOperands(); ++member) {
                                uint32_t offset = 0;
                                TryGetMemberDecorationLiteral(m_context, cursor.typeId, member,
                                                              spv::Decoration::Offset, &offset);
                                parts.push_back(BuildValue(builder, access, MemberCursor(m_context, type, member),
                                                           relWords + offset / kWordBytes));
                            }
                            return builder.AddCompositeConstruct(cursor.typeId, parts)->result_id();
                        }
                        default:
                            return 0;
                        }
                    }

                    // The mirror image: writes `valueId` into the words at
                    // `baseWordId + relWords`. `path` is the composite-extract index list that
                    // reaches the part being written, empty at the root.
                    void StoreValue(InstructionBuilder& builder, const Access& access,
                                    const TypeCursor& cursor, uint32_t relWords, uint32_t rootValueId,
                                    std::vector<uint32_t>& path) {
                        const Instruction* type = m_context->get_def_use_mgr()->GetDef(cursor.typeId);
                        switch (type->opcode()) {
                        case spv::Op::OpTypeInt:
                        case spv::Op::OpTypeFloat: {
                            const uint32_t leaf = Extract(builder, cursor.typeId, rootValueId, path);
                            if (IsDoubleType(type)) {
                                const uint32_t narrowed =
                                    builder.AddUnaryOp(m_floatTypeId, spv::Op::OpFConvert, leaf)->result_id();
                                const uint32_t bits =
                                    builder.AddUnaryOp(m_uintTypeId, spv::Op::OpBitcast, narrowed)->result_id();
                                uint32_t lo = 0;
                                uint32_t hi = 0;
                                WidenFloatBits(builder, bits, &lo, &hi);
                                StoreWord(builder, access, relWords, lo);
                                StoreWord(builder, access, relWords + 1, hi);
                                return;
                            }
                            const uint32_t word =
                                cursor.typeId == m_uintTypeId
                                    ? leaf
                                    : builder.AddUnaryOp(m_uintTypeId, spv::Op::OpBitcast, leaf)->result_id();
                            StoreWord(builder, access, relWords, word);
                            return;
                        }
                        case spv::Op::OpTypeVector: {
                            TypeCursor component;
                            component.typeId = type->GetSingleWordInOperand(0);
                            const uint32_t step = ComponentWords(component.typeId);
                            for (uint32_t i = 0; i < type->GetSingleWordInOperand(1); ++i) {
                                path.push_back(i);
                                StoreValue(builder, access, component, relWords + i * step, rootValueId, path);
                                path.pop_back();
                            }
                            return;
                        }
                        case spv::Op::OpTypeMatrix: {
                            TypeCursor column;
                            column.typeId = type->GetSingleWordInOperand(0);
                            const uint32_t step = cursor.matrixStride / kWordBytes;
                            for (uint32_t i = 0; i < type->GetSingleWordInOperand(1); ++i) {
                                path.push_back(i);
                                StoreValue(builder, access, column, relWords + i * step, rootValueId, path);
                                path.pop_back();
                            }
                            return;
                        }
                        case spv::Op::OpTypeArray: {
                            TypeCursor element = cursor;
                            element.typeId = type->GetSingleWordInOperand(0);
                            uint32_t stride = 0;
                            TryGetDecorationLiteral(m_context, cursor.typeId, spv::Decoration::ArrayStride,
                                                    &stride);
                            const uint32_t count = m_context->get_constant_mgr()
                                                       ->FindDeclaredConstant(type->GetSingleWordInOperand(1))
                                                       ->AsIntConstant()
                                                       ->GetU32BitValue();
                            for (uint32_t i = 0; i < count; ++i) {
                                path.push_back(i);
                                StoreValue(builder, access, element, relWords + i * (stride / kWordBytes),
                                           rootValueId, path);
                                path.pop_back();
                            }
                            return;
                        }
                        case spv::Op::OpTypeStruct: {
                            for (uint32_t member = 0; member < type->NumInOperands(); ++member) {
                                uint32_t offset = 0;
                                TryGetMemberDecorationLiteral(m_context, cursor.typeId, member,
                                                              spv::Decoration::Offset, &offset);
                                path.push_back(member);
                                StoreValue(builder, access, MemberCursor(m_context, type, member),
                                           relWords + offset / kWordBytes, rootValueId, path);
                                path.pop_back();
                            }
                            return;
                        }
                        default:
                            return;
                        }
                    }

                    // Replaces an OpArrayLength of an open-ended block with the element count
                    // of the ORIGINAL runtime array. The instruction now counts the words of
                    // the flattened `uint[]`, so the length is `(words - prefix) / stride`, in
                    // unsigned arithmetic and clamped at zero when the bound range does not
                    // even reach the array's offset - a wrapped subtraction would otherwise
                    // report a few billion elements. The division floors, which is what GL
                    // defines `.length()` as for a range that is not a whole number of
                    // elements. A fresh OpArrayLength is issued rather than the old one re-aimed,
                    // so the uses being redirected are never the ones the arithmetic just made.
                    void RewriteArrayLength(Instruction* arrayLength, uint32_t prefixWords, uint32_t strideWords) {
                        InstructionBuilder builder(m_context, arrayLength, kPreservedAnalyses);
                        const uint32_t variableId = arrayLength->GetSingleWordInOperand(0);
                        const uint32_t wordsId = m_context->TakeNextId();
                        builder.AddInstruction(MakeUnique<Instruction>(
                            m_context, spv::Op::OpArrayLength, m_uintTypeId, wordsId,
                            std::initializer_list<Operand>{{SPV_OPERAND_TYPE_ID, {variableId}},
                                                           {SPV_OPERAND_TYPE_LITERAL_INTEGER, {0u}}}));
                        uint32_t count = wordsId;
                        if (prefixWords != 0) {
                            const uint32_t prefixId = UintConstant(prefixWords);
                            const uint32_t past = Binary(builder, spv::Op::OpISub, m_uintTypeId, count, prefixId);
                            const uint32_t tooShort =
                                Binary(builder, spv::Op::OpULessThan, m_boolTypeId, count, prefixId);
                            count = Select(builder, tooShort, UintConstant(0), past);
                        }
                        if (strideWords != 1) {
                            count = Binary(builder, spv::Op::OpUDiv, m_uintTypeId, count,
                                           UintConstant(strideWords));
                        }
                        m_context->ReplaceAllUsesWith(arrayLength->result_id(), count);
                        m_context->KillInst(arrayLength);
                    }

                private:
                    uint32_t ComponentWords(uint32_t componentTypeId) {
                        return ScalarByteSize(m_context->get_def_use_mgr()->GetDef(componentTypeId)) /
                               kWordBytes;
                    }

                    uint32_t Binary(InstructionBuilder& builder, spv::Op opcode, uint32_t typeId, uint32_t a,
                                    uint32_t b) {
                        return builder.AddBinaryOp(typeId, opcode, a, b)->result_id();
                    }

                    uint32_t Select(InstructionBuilder& builder, uint32_t condition, uint32_t whenTrue,
                                    uint32_t whenFalse) {
                        return builder
                            .AddTernaryOp(m_uintTypeId, spv::Op::OpSelect, condition, whenTrue, whenFalse)
                            ->result_id();
                    }

                    uint32_t AsUint(InstructionBuilder& builder, uint32_t valueId) {
                        const Instruction* def = m_context->get_def_use_mgr()->GetDef(valueId);
                        if (def != nullptr && def->type_id() == m_uintTypeId) return valueId;
                        return builder.AddUnaryOp(m_uintTypeId, spv::Op::OpBitcast, valueId)->result_id();
                    }

                    // `base + words`, folding away the add when there is nothing to add.
                    uint32_t Offset(InstructionBuilder& builder, uint32_t baseWordId, uint32_t words) {
                        if (words == 0) return baseWordId;
                        return Binary(builder, spv::Op::OpIAdd, m_uintTypeId, baseWordId, UintConstant(words));
                    }

                    uint32_t LoadWord(InstructionBuilder& builder, const Access& access, uint32_t words) {
                        const uint32_t indexId = Offset(builder, access.baseWordId, words);
                        Instruction* pointer = builder.AddAccessChain(
                            access.wordPointerTypeId, access.variableId, {access.memberZeroId, indexId});
                        return builder.AddLoad(m_uintTypeId, pointer->result_id())->result_id();
                    }

                    void StoreWord(InstructionBuilder& builder, const Access& access, uint32_t words,
                                   uint32_t valueId) {
                        const uint32_t indexId = Offset(builder, access.baseWordId, words);
                        Instruction* pointer = builder.AddAccessChain(
                            access.wordPointerTypeId, access.variableId, {access.memberZeroId, indexId});
                        builder.AddStore(pointer->result_id(), valueId);
                    }

                    // One scalar of the value being stored. An empty path IS the value.
                    uint32_t Extract(InstructionBuilder& builder, uint32_t typeId, uint32_t rootValueId,
                                     const std::vector<uint32_t>& path) {
                        if (path.empty()) return rootValueId;
                        return builder.AddCompositeExtract(typeId, rootValueId, path)->result_id();
                    }

                    // binary64 word pair -> the binary32 bit pattern nearest it, truncating the
                    // mantissa bits binary32 cannot hold. Straight-line by construction: every
                    // case is an OpSelect, so this needs no control flow and never splits a block.
                    uint32_t NarrowDoubleBits(InstructionBuilder& builder, uint32_t lo, uint32_t hi) {
                        const uint32_t sign = Binary(builder, spv::Op::OpBitwiseAnd, m_uintTypeId, hi,
                                                     UintConstant(0x80000000u));
                        const uint32_t exponent = Binary(
                            builder, spv::Op::OpBitwiseAnd, m_uintTypeId,
                            Binary(builder, spv::Op::OpShiftRightLogical, m_uintTypeId, hi, UintConstant(20)),
                            UintConstant(0x7FFu));
                        const uint32_t significandHigh = Binary(builder, spv::Op::OpBitwiseAnd, m_uintTypeId,
                                                                hi, UintConstant(0xFFFFFu));
                        // The 23 bits binary32 keeps: 20 from the high word, 3 from the low one.
                        const uint32_t significand = Binary(
                            builder, spv::Op::OpBitwiseOr, m_uintTypeId,
                            Binary(builder, spv::Op::OpShiftLeftLogical, m_uintTypeId, significandHigh,
                                   UintConstant(3)),
                            Binary(builder, spv::Op::OpShiftRightLogical, m_uintTypeId, lo, UintConstant(29)));

                        // 1023 - 127 = 896, so the binary32 exponent field is `exponent - 896` and
                        // every bound on it is an unsigned comparison against that bias.
                        const uint32_t normalBits = Binary(
                            builder, spv::Op::OpBitwiseOr, m_uintTypeId, sign,
                            Binary(builder, spv::Op::OpBitwiseOr, m_uintTypeId,
                                   Binary(builder, spv::Op::OpShiftLeftLogical, m_uintTypeId,
                                          Binary(builder, spv::Op::OpISub, m_uintTypeId, exponent,
                                                 UintConstant(896)),
                                          UintConstant(23)),
                                   significand));
                        const uint32_t infinityBits = Binary(builder, spv::Op::OpBitwiseOr, m_uintTypeId, sign,
                                                             UintConstant(0x7F800000u));
                        // A quiet NaN that stays one even when every significant bit sat in the 29
                        // low bits binary32 discards.
                        const uint32_t nanBits =
                            Binary(builder, spv::Op::OpBitwiseOr, m_uintTypeId, infinityBits,
                                   Binary(builder, spv::Op::OpBitwiseOr, m_uintTypeId,
                                          UintConstant(0x400000u), significand));

                        const uint32_t significandIsZero =
                            Binary(builder, spv::Op::OpIEqual, m_boolTypeId,
                                   Binary(builder, spv::Op::OpBitwiseOr, m_uintTypeId, significandHigh, lo),
                                   UintConstant(0));
                        const uint32_t maxExponentBits =
                            Select(builder, significandIsZero, infinityBits, nanBits);

                        const uint32_t isZeroExponent =
                            Binary(builder, spv::Op::OpIEqual, m_boolTypeId, exponent, UintConstant(0));
                        const uint32_t isMaxExponent =
                            Binary(builder, spv::Op::OpIEqual, m_boolTypeId, exponent, UintConstant(0x7FFu));
                        // <= 896 is every magnitude binary32 could hold only as a subnormal;
                        // >= 1151 is every one it cannot hold at all.
                        const uint32_t underflows = Binary(builder, spv::Op::OpULessThanEqual, m_boolTypeId,
                                                           exponent, UintConstant(896));
                        const uint32_t overflows = Binary(builder, spv::Op::OpUGreaterThanEqual, m_boolTypeId,
                                                          exponent, UintConstant(1151));

                        uint32_t bits = Select(builder, overflows, infinityBits, normalBits);
                        bits = Select(builder, underflows, sign, bits);
                        bits = Select(builder, isMaxExponent, maxExponentBits, bits);
                        return Select(builder, isZeroExponent, sign, bits);
                    }

                    // The inverse: a binary32 bit pattern -> the binary64 word pair for it.
                    void WidenFloatBits(InstructionBuilder& builder, uint32_t bits, uint32_t* lo,
                                        uint32_t* hi) {
                        const uint32_t sign = Binary(builder, spv::Op::OpBitwiseAnd, m_uintTypeId, bits,
                                                     UintConstant(0x80000000u));
                        const uint32_t exponent = Binary(
                            builder, spv::Op::OpBitwiseAnd, m_uintTypeId,
                            Binary(builder, spv::Op::OpShiftRightLogical, m_uintTypeId, bits, UintConstant(23)),
                            UintConstant(0xFFu));
                        const uint32_t significand = Binary(builder, spv::Op::OpBitwiseAnd, m_uintTypeId, bits,
                                                            UintConstant(0x7FFFFFu));
                        const uint32_t significandHigh = Binary(builder, spv::Op::OpShiftRightLogical,
                                                                m_uintTypeId, significand, UintConstant(3));
                        const uint32_t significandLow = Binary(builder, spv::Op::OpShiftLeftLogical,
                                                               m_uintTypeId, significand, UintConstant(29));

                        const uint32_t normalHigh = Binary(
                            builder, spv::Op::OpBitwiseOr, m_uintTypeId, sign,
                            Binary(builder, spv::Op::OpBitwiseOr, m_uintTypeId,
                                   Binary(builder, spv::Op::OpShiftLeftLogical, m_uintTypeId,
                                          Binary(builder, spv::Op::OpIAdd, m_uintTypeId, exponent,
                                                 UintConstant(896)),
                                          UintConstant(20)),
                                   significandHigh));
                        const uint32_t maxHigh = Binary(builder, spv::Op::OpBitwiseOr, m_uintTypeId, sign,
                                                        Binary(builder, spv::Op::OpBitwiseOr, m_uintTypeId,
                                                               UintConstant(0x7FF00000u), significandHigh));

                        const uint32_t isZeroExponent =
                            Binary(builder, spv::Op::OpIEqual, m_boolTypeId, exponent, UintConstant(0));
                        const uint32_t isMaxExponent =
                            Binary(builder, spv::Op::OpIEqual, m_boolTypeId, exponent, UintConstant(0xFFu));

                        // A binary32 subnormal is below every binary64 this can name without a
                        // normalising loop, so it becomes the signed zero it is nearest to.
                        uint32_t high = Select(builder, isMaxExponent, maxHigh, normalHigh);
                        *hi = Select(builder, isZeroExponent, sign, high);
                        *lo = Select(builder, isZeroExponent, UintConstant(0), significandLow);
                    }

                    IRContext* m_context;
                    uint32_t m_uintTypeId;
                    uint32_t m_boolTypeId;
                    uint32_t m_floatTypeId;
                };
                // --- the module-level rewrite ------------------------------------------------

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

                // A fresh `uint[length]` with ArrayStride 4 - or, for an open-ended block, a
                // `uint[]` runtime array with the same stride and no length at all - spliced in
                // immediately BEFORE the block that will name it: SPIR-V has no forward
                // references between types, so appending it at the end of the section would make
                // the module invalid. A duplicate OpTypeArray or OpTypeRuntimeArray is legal
                // (SPIR-V 2.8 exempts aggregates from the uniqueness rule, and so does
                // spirv-val), so no search for an existing one is needed; the LENGTH CONSTANT is
                // not exempt, and if the module already declares it after the block there is
                // nowhere legal to put the array - the block is then declined and keeps today's
                // behaviour. Returns 0 for that; an open-ended block has no length constant to
                // place, so that reason cannot reach it.
                //
                // The `uint` element type is a different matter, and only for an OPEN-ENDED
                // block. The front end declares types in first-use order, so a block that is the
                // first thing a shader touches sits BEFORE the module's `uint` (or the module has
                // none, and the one the pass asked for was appended at the end). Declining there
                // would send exactly the buffers this rewrite exists for back to the demotion on
                // nothing but where they stand in the source. OpTypeInt has no operands, and
                // nothing that names it can precede where it was, so moving it up in front of the
                // block is always legal. A BOUNDED block keeps declining instead: that is what it
                // has always done, and widening it is a change to a path this one does not need.
                //
                // NOTHING IS WRITTEN until every reason to decline has been ruled out, so a block
                // this returns 0 for leaves the module as it found it - which is what lets
                // Process() truthfully report SuccessWithoutChange for a module of only those.
                uint32_t CreateWordArrayTypeBefore(IRContext* context, Instruction* structType,
                                                   uint32_t uintTypeId, uint32_t length, bool openEnded) {
                    Instruction* uintType = context->get_def_use_mgr()->GetDef(uintTypeId);
                    if (uintType == nullptr || uintType->opcode() != spv::Op::OpTypeInt) return 0;
                    const bool hoistUint = !DeclaredBefore(context, uintTypeId, structType->result_id());
                    if (hoistUint && !openEnded) return 0;

                    uint32_t lengthConstantId = 0;
                    if (!openEnded) {
                        auto* constantMgr = context->get_constant_mgr();
                        const spvtools::opt::analysis::Type* uintDescriptor =
                            context->get_type_mgr()->GetType(uintTypeId);
                        if (uintDescriptor == nullptr) return 0;
                        const spvtools::opt::analysis::Constant* lengthConstant =
                            constantMgr->GetConstant(uintDescriptor, {length});
                        if (lengthConstant == nullptr) return 0;

                        Module::inst_iterator position = PositionOf(context, structType);
                        if (position == context->types_values_end()) return 0;
                        // Created in front of the block when it is not there yet, so the only way
                        // this declines is a constant the module already declares after it.
                        Instruction* lengthInst =
                            constantMgr->GetDefiningInstruction(lengthConstant, 0, &position);
                        if (lengthInst == nullptr) return 0;
                        if (!DeclaredBefore(context, lengthInst->result_id(), structType->result_id())) return 0;
                        lengthConstantId = lengthInst->result_id();
                    }

                    const uint32_t arrayTypeId = context->TakeNextId();
                    if (arrayTypeId == 0) return 0;

                    if (hoistUint) uintType->InsertBefore(structType);
                    std::unique_ptr<Instruction> arrayType =
                        openEnded
                            ? MakeUnique<Instruction>(
                                  context, spv::Op::OpTypeRuntimeArray, 0, arrayTypeId,
                                  std::initializer_list<Operand>{{SPV_OPERAND_TYPE_ID, {uintTypeId}}})
                            : MakeUnique<Instruction>(
                                  context, spv::Op::OpTypeArray, 0, arrayTypeId,
                                  std::initializer_list<Operand>{{SPV_OPERAND_TYPE_ID, {uintTypeId}},
                                                                 {SPV_OPERAND_TYPE_ID, {lengthConstantId}}});
                    Instruction* inserted = structType->InsertBefore(std::move(arrayType));
                    context->AnalyzeDefUse(inserted);
                    context->get_decoration_mgr()->AddDecorationVal(
                        arrayTypeId, static_cast<uint32_t>(spv::Decoration::ArrayStride), kWordBytes);
                    return arrayTypeId;
                }

                // The access qualifiers the collapsed member has to keep. Coherent and Volatile
                // are taken from ANY member that had them - dropping one could lose a write
                // another invocation has to see - while Restrict, NonWritable and NonReadable are
                // taken only from ALL of them, because each is a promise the shader would break
                // if one member never made it.
                std::vector<spv::Decoration> SurvivingAccessQualifiers(IRContext* context, uint32_t structId,
                                                                      uint32_t memberCount) {
                    static constexpr spv::Decoration kAny[] = {spv::Decoration::Coherent,
                                                               spv::Decoration::Volatile};
                    static constexpr spv::Decoration kAll[] = {spv::Decoration::Restrict,
                                                              spv::Decoration::NonWritable,
                                                              spv::Decoration::NonReadable};
                    std::vector<spv::Decoration> surviving;
                    for (const spv::Decoration kind : kAny) {
                        for (uint32_t member = 0; member < memberCount; ++member) {
                            if (HasMemberDecoration(context, structId, member, kind)) {
                                surviving.push_back(kind);
                                break;
                            }
                        }
                    }
                    for (const spv::Decoration kind : kAll) {
                        bool all = memberCount > 0;
                        for (uint32_t member = 0; member < memberCount && all; ++member) {
                            all = HasMemberDecoration(context, structId, member, kind);
                        }
                        if (all) surviving.push_back(kind);
                    }
                    return surviving;
                }

                // Drops every OpMemberDecorate and OpMemberName the collapsed struct no longer has
                // a member for - which is all of them, since the one member it keeps is a
                // different thing entirely from the one that used to be member 0.
                void StripMemberAnnotations(IRContext* context, uint32_t structId) {
                    std::vector<Instruction*> doomed;
                    for (Instruction* decoration :
                         context->get_decoration_mgr()->GetDecorationsFor(structId, false)) {
                        if (decoration->opcode() == spv::Op::OpMemberDecorate) doomed.push_back(decoration);
                    }
                    for (Instruction& debug : context->module()->debugs2()) {
                        if (debug.opcode() != spv::Op::OpMemberName || debug.NumInOperands() < 2) continue;
                        if (debug.GetSingleWordInOperand(0) != structId) continue;
                        doomed.push_back(&debug);
                    }
                    for (Instruction* inst : doomed) context->KillInst(inst);
                }

                // Plans every shader storage block in the module that holds a 64-bit float and
                // that this pass can rewrite exactly. Reads the module and never writes it.
                std::vector<BlockPlan> BuildPlans(IRContext* context) {
                    std::vector<BlockPlan> plans;
                    auto* defUseMgr = context->get_def_use_mgr();

                    // Declaration order, so a module with two candidate blocks is rewritten the
                    // same way every time it is compiled.
                    std::vector<uint32_t> structOrder;
                    std::unordered_map<uint32_t, std::vector<Instruction*>> variablesByStruct;
                    std::unordered_map<uint32_t, uint32_t> storageClassByStruct;
                    for (Instruction& inst : context->module()->types_values()) {
                        if (inst.opcode() != spv::Op::OpVariable || inst.NumInOperands() < 1) continue;
                        const uint32_t storageClass = inst.GetSingleWordInOperand(0);
                        const bool isStorageBufferClass =
                            storageClass == static_cast<uint32_t>(spv::StorageClass::StorageBuffer);
                        const bool isUniformClass =
                            storageClass == static_cast<uint32_t>(spv::StorageClass::Uniform);
                        if (!isStorageBufferClass && !isUniformClass) continue;

                        Instruction* pointerType = defUseMgr->GetDef(inst.type_id());
                        if (pointerType == nullptr || pointerType->opcode() != spv::Op::OpTypePointer) continue;
                        const uint32_t structId = pointerType->GetSingleWordInOperand(1);
                        Instruction* structType = defUseMgr->GetDef(structId);
                        if (structType == nullptr || structType->opcode() != spv::Op::OpTypeStruct ||
                            structType->NumInOperands() == 0) {
                            continue;
                        }
                        // A shader storage block is spelled Block + StorageBuffer from SPIR-V 1.3
                        // and BufferBlock + Uniform before it; a plain UNIFORM block is neither,
                        // and is deliberately left to the demotion - the frontend's own uniform
                        // routing reflects the module that pass produces.
                        const bool isStorageBlock =
                            (isStorageBufferClass && HasDecoration(context, structId, spv::Decoration::Block)) ||
                            (isUniformClass && HasDecoration(context, structId, spv::Decoration::BufferBlock));
                        if (!isStorageBlock) continue;

                        std::unordered_set<uint32_t> visiting;
                        if (!TypeContainsFloat64(context, structId, visiting)) continue;

                        if (variablesByStruct.find(structId) == variablesByStruct.end()) {
                            structOrder.push_back(structId);
                            storageClassByStruct[structId] = storageClass;
                        }
                        variablesByStruct[structId].push_back(&inst);
                    }

                    // A struct type is not owned by the variables that happen to be storage blocks:
                    // if ANY other variable points at the same one, collapsing it would rewrite a
                    // declaration this pass never looked at. Count every variable of every struct
                    // and require the two counts to agree.
                    std::unordered_map<uint32_t, uint32_t> variableCountByStruct;
                    for (Instruction& inst : context->module()->types_values()) {
                        if (inst.opcode() != spv::Op::OpVariable) continue;
                        Instruction* pointerType = defUseMgr->GetDef(inst.type_id());
                        if (pointerType == nullptr || pointerType->opcode() != spv::Op::OpTypePointer) continue;
                        ++variableCountByStruct[pointerType->GetSingleWordInOperand(1)];
                    }
                    for (auto it = structOrder.begin(); it != structOrder.end();) {
                        if (variableCountByStruct[*it] == variablesByStruct[*it].size()) {
                            ++it;
                            continue;
                        }
                        MGLOG_D("[spirv] storage block %%%u holds a double but its struct type is shared "
                                "with a declaration that is not one; left to the fp64 demotion",
                                *it);
                        it = structOrder.erase(it);
                    }

                    for (const uint32_t structId : structOrder) {
                        Instruction* structType = defUseMgr->GetDef(structId);
                        TypeCursor blockCursor;
                        blockCursor.typeId = structId;
                        uint32_t blockBytes = 0;
                        bool openEnded = false;
                        uint32_t tailStrideBytes = 0;
                        // An open-ended block whose runtime array is its only member measures a
                        // prefix of 0 bytes and is perfectly describable; only a BOUNDED block of
                        // no bytes is not, and MeasureBlock already refuses to size one of those.
                        if (!MeasureBlock(context, structType, &blockBytes, &openEnded, &tailStrideBytes) ||
                            blockBytes % kWordBytes != 0 || (!openEnded && blockBytes == 0)) {
                            MGLOG_D("[spirv] storage block %%%u holds a double but its byte layout cannot be "
                                    "described exactly; left to the fp64 demotion",
                                    structId);
                            continue;
                        }

                        BlockPlan plan;
                        plan.structType = structType;
                        plan.storageClass = storageClassByStruct[structId];
                        plan.wordCount = blockBytes / kWordBytes;
                        plan.openEnded = openEnded;
                        plan.tailStrideWords = tailStrideBytes / kWordBytes;
                        const uint32_t lastMember = structType->NumInOperands() - 1;

                        bool expressible = true;
                        for (Instruction* variable : variablesByStruct[structId]) {
                            std::vector<Instruction*> chains;
                            std::unordered_set<uint32_t> seenChains;
                            std::unordered_set<uint32_t> seenLengths;
                            defUseMgr->ForEachUser(variable, [&](Instruction* user) {
                                if (!expressible) return;
                                switch (user->opcode()) {
                                case spv::Op::OpName:
                                case spv::Op::OpDecorate:
                                case spv::Op::OpDecorateId:
                                case spv::Op::OpEntryPoint:
                                    return;
                                case spv::Op::OpAccessChain:
                                case spv::Op::OpInBoundsAccessChain:
                                    if (user->NumInOperands() >= 1 &&
                                        user->GetSingleWordInOperand(0) == variable->result_id()) {
                                        if (seenChains.insert(user->result_id()).second) chains.push_back(user);
                                        return;
                                    }
                                    expressible = false;
                                    return;
                                case spv::Op::OpArrayLength: {
                                    // Only an open-ended block has a length to ask for, and
                                    // only of its last member; the result has to be the 32-bit
                                    // uint the rewrite's arithmetic is typed in, which is the
                                    // only result type the instruction allows anyway.
                                    const Instruction* resultType = defUseMgr->GetDef(user->type_id());
                                    const bool isUint = resultType != nullptr &&
                                                        resultType->opcode() == spv::Op::OpTypeInt &&
                                                        resultType->GetSingleWordInOperand(0) == 32u &&
                                                        resultType->GetSingleWordInOperand(1) == 0u;
                                    if (openEnded && isUint && user->NumInOperands() >= 2 &&
                                        user->GetSingleWordInOperand(0) == variable->result_id() &&
                                        user->GetSingleWordInOperand(1) == lastMember) {
                                        if (seenLengths.insert(user->result_id()).second) {
                                            plan.arrayLengths.push_back(user);
                                        }
                                        return;
                                    }
                                    expressible = false;
                                    return;
                                }
                                default:
                                    expressible = false;
                                    return;
                                }
                            });
                            if (!expressible) break;

                            for (Instruction* chain : chains) {
                                ChainPlan chainPlan;
                                chainPlan.chain = chain;
                                chainPlan.variableId = variable->result_id();
                                if (!PlanChain(context, chain, blockCursor, &chainPlan)) {
                                    expressible = false;
                                    break;
                                }
                                uint32_t leafCount = 0;
                                if (!CanDecompose(context, chainPlan.pointee, &leafCount)) {
                                    expressible = false;
                                    break;
                                }
                                std::unordered_set<uint32_t> seenUses;
                                defUseMgr->ForEachUser(chain, [&](Instruction* user) {
                                    if (!expressible) return;
                                    if (user->opcode() == spv::Op::OpLoad &&
                                        user->GetSingleWordInOperand(0) == chain->result_id()) {
                                        if (seenUses.insert(user->unique_id()).second) {
                                            chainPlan.loads.push_back(user);
                                        }
                                        return;
                                    }
                                    if (user->opcode() == spv::Op::OpStore && user->NumInOperands() >= 2 &&
                                        user->GetSingleWordInOperand(0) == chain->result_id() &&
                                        user->GetSingleWordInOperand(1) != chain->result_id()) {
                                        if (seenUses.insert(user->unique_id()).second) {
                                            chainPlan.stores.push_back(user);
                                        }
                                        return;
                                    }
                                    expressible = false;
                                });
                                if (!expressible) break;
                                plan.chains.push_back(std::move(chainPlan));
                            }
                            if (!expressible) break;
                        }
                        if (!expressible) {
                            MGLOG_D("[spirv] storage block %%%u holds a double but is reached in a way this "
                                    "pass cannot re-address; left to the fp64 demotion",
                                    structId);
                            continue;
                        }
                        plans.push_back(std::move(plan));
                    }
                    return plans;
                }
            } // namespace

            spvtools::opt::Pass::Status FlattenFloat64StorageBlockPass::Process() {
                auto* irContext = context();
                std::vector<BlockPlan> plans = BuildPlans(irContext);
                if (plans.empty()) {
                    return Status::SuccessWithoutChange;
                }

                spvtools::opt::analysis::Integer uintDescriptor(32, false);
                spvtools::opt::analysis::Bool boolDescriptor;
                spvtools::opt::analysis::Float floatDescriptor(32);
                const uint32_t uintTypeId = irContext->get_type_mgr()->GetTypeInstruction(&uintDescriptor);
                const uint32_t boolTypeId = irContext->get_type_mgr()->GetTypeInstruction(&boolDescriptor);
                const uint32_t floatTypeId = irContext->get_type_mgr()->GetTypeInstruction(&floatDescriptor);
                if (uintTypeId == 0 || boolTypeId == 0 || floatTypeId == 0) {
                    return Status::SuccessWithoutChange;
                }

                Emitter emitter(irContext, uintTypeId, boolTypeId, floatTypeId);
                bool modified = false;
                // Every block declines before anything is written for it, so |touched| only ever
                // parts company with |modified| on a shape that cannot happen without the module
                // running out of ids - and even then the status must not claim the bytes are
                // untouched, because the caller relies on that to skip invalidating its analyses.
                bool touched = false;
                for (BlockPlan& plan : plans) {
                    const uint32_t structId = plan.structType->result_id();
                    const uint32_t arrayTypeId = CreateWordArrayTypeBefore(
                        irContext, plan.structType, uintTypeId, plan.wordCount, plan.openEnded);
                    if (arrayTypeId == 0) {
                        // Nothing was written for it, so the module is still the one that came in.
                        MGLOG_D("[spirv] storage block %%%u: no legal place for the flattened word array; "
                                "left to the fp64 demotion",
                                structId);
                        continue;
                    }
                    // Past this point the module HAS been written to, so an abandoned block would
                    // leave a dead type behind - the status has to say so even then.
                    touched = true;
                    const uint32_t wordPointerTypeId = irContext->get_type_mgr()->FindPointerToType(
                        uintTypeId, static_cast<spv::StorageClass>(plan.storageClass));
                    if (wordPointerTypeId == 0) continue;
                    const uint32_t memberZeroId = emitter.UintConstant(0);

                    for (ChainPlan& chainPlan : plan.chains) {
                        Emitter::Access access;
                        access.variableId = chainPlan.variableId;
                        access.wordPointerTypeId = wordPointerTypeId;
                        access.memberZeroId = memberZeroId;
                        access.baseWordId = emitter.WordIndexOf(chainPlan);

                        for (Instruction* load : chainPlan.loads) {
                            InstructionBuilder builder(irContext, load, kPreservedAnalyses);
                            const uint32_t rebuilt = emitter.BuildValue(builder, access, chainPlan.pointee, 0);
                            irContext->ReplaceAllUsesWith(load->result_id(), rebuilt);
                            irContext->KillInst(load);
                        }
                        for (Instruction* store : chainPlan.stores) {
                            InstructionBuilder builder(irContext, store, kPreservedAnalyses);
                            std::vector<uint32_t> path;
                            emitter.StoreValue(builder, access, chainPlan.pointee, 0,
                                               store->GetSingleWordInOperand(1), path);
                            irContext->KillInst(store);
                        }
                        irContext->KillInst(chainPlan.chain);
                    }
                    for (Instruction* arrayLength : plan.arrayLengths) {
                        emitter.RewriteArrayLength(arrayLength, plan.wordCount, plan.tailStrideWords);
                    }

                    const std::vector<spv::Decoration> surviving = SurvivingAccessQualifiers(
                        irContext, structId, plan.structType->NumInOperands());
                    StripMemberAnnotations(irContext, structId);
                    plan.structType->SetInOperands({{SPV_OPERAND_TYPE_ID, {arrayTypeId}}});
                    irContext->UpdateDefUse(plan.structType);
                    irContext->get_decoration_mgr()->AddMemberDecoration(
                        structId, 0u, static_cast<uint32_t>(spv::Decoration::Offset), 0u);
                    for (const spv::Decoration kind : surviving) {
                        // AddMemberDecoration always carries a literal value; these have none, so
                        // the instruction has to be spelled out.
                        irContext->get_decoration_mgr()->AddDecoration(
                            spv::Op::OpMemberDecorate,
                            {{SPV_OPERAND_TYPE_ID, {structId}},
                             {SPV_OPERAND_TYPE_LITERAL_INTEGER, {0u}},
                             {SPV_OPERAND_TYPE_DECORATION, {static_cast<uint32_t>(kind)}}});
                    }
                    modified = true;
                    if (plan.openEnded) {
                        MGLOG_D("[spirv] storage block %%%u: flattened into an open-ended word array (%u-word "
                                "prefix, %u-word elements) so its 64-bit members keep the byte layout the "
                                "application bound",
                                structId, plan.wordCount, plan.tailStrideWords);
                    } else {
                        MGLOG_D("[spirv] storage block %%%u: flattened into %u words so its 64-bit members "
                                "keep the byte layout the application bound",
                                structId, plan.wordCount);
                    }
                }

                if (!modified && !touched) {
                    return Status::SuccessWithoutChange;
                }
                irContext->InvalidateAnalysesExceptFor(IRContext::kAnalysisNone);
                return Status::SuccessWithChange;
            }

            spvtools::Optimizer::PassToken
            FlattenFloat64StorageBlockPass::CreateFlattenFloat64StorageBlockPass() {
                return spvtools::Optimizer::PassToken(MakeUnique<FlattenFloat64StorageBlockPass>());
            }
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
