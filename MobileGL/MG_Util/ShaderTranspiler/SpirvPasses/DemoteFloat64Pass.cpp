// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/DemoteFloat64Pass.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "DemoteFloat64Pass.h"

#include "spirv.hpp"
#include "source/latest_version_glsl_std_450_header.h"
#include "source/opt/def_use_manager.h"
#include "source/opt/instruction.h"
#include "source/opt/ir_context.h"
#include "source/opt/module.h"
#include "source/util/make_unique.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace MobileGL {
    namespace MG_Util {
        namespace ShaderTranspiler {
            namespace {
                using spvtools::opt::IRContext;
                using spvtools::opt::Instruction;
                using spvtools::opt::Operand;

                constexpr Uint32 kFloat64Width = 64;
                constexpr Uint32 kFloat32Width = 32;

                // OpTypeFloat <id> <Width>. SPIR-V 1.6 added an optional FP Encoding operand after
                // it; the width stays operand 0 either way, and an encoded (non-IEEE) float is not
                // something glslang can emit for `double`, so it is left alone by the width guard.
                Bool IsFloatTypeOfWidth(const Instruction& type, Uint32 width) {
                    return type.opcode() == spv::Op::OpTypeFloat && type.NumInOperands() >= 1 &&
                           type.GetSingleWordInOperand(0) == width;
                }

                // Exactly the types spirv-val forbids a second declaration of. Aggregates - arrays
                // and structs - are excluded on purpose: the spec permits duplicates of those, and
                // merging them would take one of the two OpNames and one of the two ArrayStride /
                // Offset decoration sets with it.
                Bool IsDuplicableType(spv::Op opcode) {
                    switch (opcode) {
                        case spv::Op::OpTypeFloat:
                        case spv::Op::OpTypeVector:
                        case spv::Op::OpTypeMatrix:
                        case spv::Op::OpTypePointer:
                        case spv::Op::OpTypeFunction: return true;
                        default: return false;
                    }
                }

                Uint32 RoundUp(Uint32 value, Uint32 alignment) {
                    if (alignment == 0) return value;
                    return (value + alignment - 1) / alignment * alignment;
                }

                // Re-derives the std140 / std430 layout of a block whose members have just become
                // 32 bits wide, and writes it back as Offset / ArrayStride / MatrixStride.
                //
                // Why the layout is recomputed rather than preserved. Preserving it - leaving each
                // member at the byte offset glslang picked for the 64-bit type and letting the
                // freed 4 bytes become padding - keeps the application's byte layout intact and is
                // the obvious first choice, but it does not survive contact with the Espryt path:
                // SPIRV-Cross has to print an ESSL block, GLSL ES has no member `layout(offset=)`
                // (no ARB_enhanced_layouts), so it refuses any block whose declared offsets are not
                // exactly what std140 or std430 computes - "Buffer block cannot be expressed as any
                // of std430, std140, scalar". Every shader with a double in a block would fail to
                // transpile at all, which is the case this whole demotion exists to fix. Padding
                // members back in cannot rescue it either: a dmat4 member carries MatrixStride 32
                // and std140 demands 16 for the demoted mat4, and no amount of padding BETWEEN
                // members changes a stride INSIDE one.
                //
                // What recomputing costs: a block laid out for 64-bit members changes its
                // driver-visible byte layout, so an application that hard-codes std140 offsets
                // computed for doubles addresses the wrong bytes. Applications that query their
                // offsets are unaffected, and MobileGL's own default-uniform block is unaffected by
                // construction - the frontend builds its uniform routing by reflecting THIS module
                // (ProgramSpirvTask::BuildGlobalUboRouting), so glUniform*d writes wherever the
                // demoted shader reads.
                class BlockRelayout {
                public:
                    BlockRelayout(IRContext* irContext, Bool std140)
                        : m_irContext(irContext), m_std140(std140) {}

                    // Size and alignment of `typeId`, applying every stride decoration it implies
                    // on the way down. Zero size means "not a type this layout knows how to
                    // describe"; the caller then leaves the block alone rather than guessing.
                    struct Extent {
                        Uint32 size = 0;
                        Uint32 alignment = 0;
                    };

                    Extent Measure(Uint32 typeId) {
                        const auto memo = m_extents.find(typeId);
                        if (memo != m_extents.end()) return memo->second;

                        const Extent extent = MeasureUncached(typeId);
                        m_extents.emplace(typeId, extent);
                        return extent;
                    }

                private:
                    Extent MeasureUncached(Uint32 typeId) {
                        const Instruction* type = m_irContext->get_def_use_mgr()->GetDef(typeId);
                        if (type == nullptr) return {};

                        switch (type->opcode()) {
                            case spv::Op::OpTypeInt:
                            case spv::Op::OpTypeFloat: {
                                const Uint32 bytes = type->GetSingleWordInOperand(0) / 8;
                                return {bytes, bytes};
                            }
                            case spv::Op::OpTypeBool: return {4, 4};
                            case spv::Op::OpTypeVector: {
                                const Extent component = Measure(type->GetSingleWordInOperand(0));
                                if (component.size == 0) return {};
                                const Uint32 count = type->GetSingleWordInOperand(1);
                                // A three-component vector aligns like a four-component one.
                                return {component.size * count,
                                        component.alignment * (count == 3 ? 4 : count)};
                            }
                            case spv::Op::OpTypeMatrix: {
                                const Extent column = Measure(type->GetSingleWordInOperand(0));
                                if (column.size == 0) return {};
                                const Uint32 stride = MatrixOrArrayStride(column.alignment);
                                return {stride * type->GetSingleWordInOperand(1), stride};
                            }
                            case spv::Op::OpTypeArray:
                            case spv::Op::OpTypeRuntimeArray: {
                                const Extent element = Measure(type->GetSingleWordInOperand(0));
                                if (element.size == 0) return {};
                                const Uint32 alignment = MatrixOrArrayStride(element.alignment);
                                const Uint32 stride = RoundUp(element.size, alignment);
                                SetTypeDecoration(typeId, spv::Decoration::ArrayStride, stride);
                                if (type->opcode() == spv::Op::OpTypeRuntimeArray) {
                                    // An unsized array contributes its stride and nothing more; the
                                    // block's size is whatever the application bound.
                                    return {stride, alignment};
                                }
                                return {stride * ArrayLength(type->GetSingleWordInOperand(1)), alignment};
                            }
                            case spv::Op::OpTypeStruct: return MeasureStruct(*type);
                            default: return {};
                        }
                    }

                    Extent MeasureStruct(const Instruction& structType) {
                        Uint32 cursor = 0;
                        Uint32 alignment = m_std140 ? 16u : 1u;
                        for (Uint32 member = 0; member < structType.NumInOperands(); ++member) {
                            const Uint32 memberTypeId = structType.GetSingleWordInOperand(member);
                            const Extent extent = Measure(memberTypeId);
                            if (extent.size == 0) return {};

                            const Uint32 offset = RoundUp(cursor, extent.alignment);
                            SetMemberDecoration(structType.result_id(), member, spv::Decoration::Offset, offset);
                            // A matrix member carries the stride between its columns on the MEMBER,
                            // not on the type, so it has to be (re)stated here - including for a
                            // matrix reached through an array.
                            const Instruction* memberType =
                                m_irContext->get_def_use_mgr()->GetDef(PeelArrays(memberTypeId));
                            if (memberType != nullptr && memberType->opcode() == spv::Op::OpTypeMatrix) {
                                const Extent column = Measure(memberType->GetSingleWordInOperand(0));
                                SetMemberDecoration(structType.result_id(), member,
                                                    spv::Decoration::MatrixStride,
                                                    MatrixOrArrayStride(column.alignment));
                            }

                            cursor = offset + extent.size;
                            alignment = std::max(alignment, extent.alignment);
                        }
                        return {RoundUp(cursor, alignment), alignment};
                    }

                    // std140 rounds every array and matrix stride up to a four-component vector.
                    Uint32 MatrixOrArrayStride(Uint32 elementAlignment) const {
                        return m_std140 ? RoundUp(elementAlignment, 16) : elementAlignment;
                    }

                    Uint32 PeelArrays(Uint32 typeId) const {
                        const Instruction* type = m_irContext->get_def_use_mgr()->GetDef(typeId);
                        while (type != nullptr && (type->opcode() == spv::Op::OpTypeArray ||
                                                   type->opcode() == spv::Op::OpTypeRuntimeArray)) {
                            type = m_irContext->get_def_use_mgr()->GetDef(type->GetSingleWordInOperand(0));
                        }
                        return type != nullptr ? type->result_id() : 0;
                    }

                    Uint32 ArrayLength(Uint32 lengthConstantId) const {
                        const Instruction* length = m_irContext->get_def_use_mgr()->GetDef(lengthConstantId);
                        if (length == nullptr || length->opcode() != spv::Op::OpConstant ||
                            length->NumInOperands() < 1) {
                            return 1;
                        }
                        return length->GetSingleWordInOperand(0);
                    }

                    void SetTypeDecoration(Uint32 targetId, spv::Decoration decoration, Uint32 value) {
                        for (Instruction& annotation : m_irContext->annotations()) {
                            if (annotation.opcode() != spv::Op::OpDecorate) continue;
                            if (annotation.GetSingleWordInOperand(0) != targetId) continue;
                            if (static_cast<spv::Decoration>(annotation.GetSingleWordInOperand(1)) != decoration) {
                                continue;
                            }
                            annotation.SetInOperand(2, {value});
                            return;
                        }
                    }

                    void SetMemberDecoration(Uint32 structId, Uint32 member, spv::Decoration decoration,
                                             Uint32 value) {
                        for (Instruction& annotation : m_irContext->annotations()) {
                            if (annotation.opcode() != spv::Op::OpMemberDecorate) continue;
                            if (annotation.GetSingleWordInOperand(0) != structId) continue;
                            if (annotation.GetSingleWordInOperand(1) != member) continue;
                            if (static_cast<spv::Decoration>(annotation.GetSingleWordInOperand(2)) != decoration) {
                                continue;
                            }
                            annotation.SetInOperand(3, {value});
                            return;
                        }
                    }

                    IRContext* m_irContext = nullptr;
                    Bool m_std140 = true;
                    std::unordered_map<Uint32, Extent> m_extents;
                };
            } // namespace

            spvtools::opt::Pass::Status DemoteFloat64Pass::Process() {
                auto* irContext = context();
                auto* defUseMgr = irContext->get_def_use_mgr();

                // --- 1. the leaf types -------------------------------------------------------
                std::vector<Instruction*> float64Types;
                for (Instruction& type : irContext->types_values()) {
                    if (IsFloatTypeOfWidth(type, kFloat64Width)) {
                        float64Types.push_back(&type);
                    }
                }
                if (float64Types.empty()) return Status::SuccessWithoutChange;

                std::unordered_set<Uint32> float64TypeIds;
                for (const Instruction* type : float64Types) {
                    float64TypeIds.insert(type->result_id());
                }

                // Every type a *value* can have that is 64-bit float underneath: the scalar itself
                // plus the vectors and matrices built from it. The types-and-values section is in
                // declaration order and SPIR-V forbids a type from forward-referencing another, so
                // one forward walk is already the complete transitive closure. Aggregates and
                // pointers are deliberately not included: no operand of the instructions checked
                // below is ever an aggregate or a pointer.
                std::unordered_set<Uint32> float64ValueTypeIds = float64TypeIds;
                for (Instruction& type : irContext->types_values()) {
                    if (type.opcode() != spv::Op::OpTypeVector && type.opcode() != spv::Op::OpTypeMatrix) {
                        continue;
                    }
                    if (float64ValueTypeIds.count(type.GetSingleWordInOperand(0)) != 0) {
                        float64ValueTypeIds.insert(type.result_id());
                    }
                }

                // Every type that has a 64-bit float anywhere underneath it, which is exactly the
                // set of blocks whose layout has to be re-derived once the leaves narrow. Computed
                // now, before the rewrite makes a demoted float indistinguishable from one that was
                // always 32 bits; the same single forward walk is a complete closure.
                std::unordered_set<Uint32> wideTypeIds = float64TypeIds;
                for (Instruction& type : irContext->types_values()) {
                    const auto contains = [&](Uint32 operand) {
                        return wideTypeIds.count(type.GetSingleWordInOperand(operand)) != 0;
                    };
                    switch (type.opcode()) {
                        case spv::Op::OpTypeVector:
                        case spv::Op::OpTypeMatrix:
                        case spv::Op::OpTypeArray:
                        case spv::Op::OpTypeRuntimeArray:
                            if (contains(0)) wideTypeIds.insert(type.result_id());
                            break;
                        case spv::Op::OpTypePointer:
                            if (contains(1)) wideTypeIds.insert(type.result_id());
                            break;
                        case spv::Op::OpTypeStruct:
                            for (Uint32 member = 0; member < type.NumInOperands(); ++member) {
                                if (contains(member)) {
                                    wideTypeIds.insert(type.result_id());
                                    break;
                                }
                            }
                            break;
                        default: break;
                    }
                }

                const auto valueIsFloat64 = [&](Uint32 id) {
                    const Instruction* def = defUseMgr->GetDef(id);
                    return def != nullptr && float64ValueTypeIds.count(def->type_id()) != 0;
                };

                // --- 2. decline before touching anything ------------------------------------
                // These are the operations that mean "the 64 bits themselves", not "a wide float".
                // Narrowing one side of them produces a module spirv-val rejects, and rebuilding
                // the value would mean emulating fp64 in software. Bail out with the module
                // byte-identical instead; the caller still has its "module declares Float64"
                // diagnostic for it.
                Uint32 glslStd450SetId = 0;
                for (const Instruction& import : irContext->ext_inst_imports()) {
                    if (import.GetInOperand(0).AsString() == "GLSL.std.450") {
                        glslStd450SetId = import.result_id();
                        break;
                    }
                }

                const char* declineReason = nullptr;
                irContext->module()->ForEachInst(
                    [&](Instruction* inst) {
                        if (declineReason != nullptr) return;
                        switch (inst->opcode()) {
                            case spv::Op::OpBitcast: {
                                // "Total bit width of Result Type and Operand must match" - true
                                // today, false the moment one of the two sides halves.
                                const Bool resultIs64 = float64ValueTypeIds.count(inst->type_id()) != 0;
                                const Bool operandIs64 = valueIsFloat64(inst->GetSingleWordInOperand(0));
                                if (resultIs64 != operandIs64) {
                                    declineReason = "an OpBitcast across the 64-bit boundary "
                                                    "(doubleBitsToUint64 / uint64BitsToDouble / "
                                                    "packDouble2x32)";
                                }
                                break;
                            }
                            case spv::Op::OpExtInst: {
                                if (glslStd450SetId == 0 ||
                                    inst->GetSingleWordInOperand(0) != glslStd450SetId) {
                                    break;
                                }
                                const Uint32 extOpcode = inst->GetSingleWordInOperand(1);
                                if (extOpcode == GLSLstd450PackDouble2x32 ||
                                    extOpcode == GLSLstd450UnpackDouble2x32) {
                                    declineReason = "GLSL.std.450 PackDouble2x32 / UnpackDouble2x32, "
                                                    "which are defined only for a 64-bit float";
                                }
                                break;
                            }
                            default: break;
                        }
                    },
                    /*run_on_debug_line_insts=*/false);

                if (declineReason != nullptr) {
                    MGLOG_D("DemoteFloat64Pass: declined - the module uses %s; its 64-bit floats are "
                            "left in place",
                            declineReason);
                    return Status::SuccessWithoutChange;
                }

                // --- 3. re-encode the literals ----------------------------------------------
                // A 64-bit float constant carries two literal words and a 32-bit one carries a
                // single word, so the value has to be narrowed before the type changes underneath
                // it - afterwards there is no way left to tell how wide the literal was meant to
                // be. Composite constants hold <id>s, not literals, and need nothing.
                for (Instruction& value : irContext->types_values()) {
                    if (value.opcode() != spv::Op::OpConstant && value.opcode() != spv::Op::OpSpecConstant) {
                        continue;
                    }
                    if (float64TypeIds.count(value.type_id()) == 0) continue;
                    if (value.NumInOperands() < 1) continue;
                    const Operand& literal = value.GetInOperand(0);
                    if (literal.words.size() != 2) continue;

                    const Uint64 bits =
                        (static_cast<Uint64>(literal.words[1]) << 32) | static_cast<Uint64>(literal.words[0]);
                    double wide = 0.0;
                    std::memcpy(&wide, &bits, sizeof(wide));
                    // Deliberately the ordinary narrowing conversion: a magnitude no float can
                    // hold becomes an infinity, which is the same answer the demoted arithmetic
                    // around it would produce.
                    const float narrow = static_cast<float>(wide);
                    Uint32 narrowedBits = 0;
                    std::memcpy(&narrowedBits, &narrow, sizeof(narrowedBits));

                    Operand narrowedLiteral = literal;
                    narrowedLiteral.words = {narrowedBits};
                    value.SetInOperands({std::move(narrowedLiteral)});
                }

                // --- 4. the demotion itself --------------------------------------------------
                // In place, so every composite type, every pointer, every struct member offset and
                // every debug name that referred to the 64-bit type keeps referring to the same
                // <id>. This is the whole reason the pass does not build parallel types.
                for (Instruction* type : float64Types) {
                    type->SetInOperand(0, {kFloat32Width});
                }
                // The cached analysis::Type objects were built against the old widths.
                irContext->InvalidateAnalyses(IRContext::kAnalysisTypes);

                // --- 5. the conversions that just became identities --------------------------
                // `float(someDouble)` and `double(someFloat)` are both OpFConvert, and SPIR-V
                // requires the two component widths to differ. Both sides are 32 bits now, so each
                // one is replaced by its operand. Walking in module order means a chain of them
                // resolves in a single sweep: by the time the second is reached its operand has
                // already been rewritten to the ultimate source.
                const auto componentWidth = [&](Uint32 typeId) -> Uint32 {
                    const Instruction* def = defUseMgr->GetDef(typeId);
                    if (def == nullptr) return 0;
                    if (def->opcode() == spv::Op::OpTypeVector) {
                        def = defUseMgr->GetDef(def->GetSingleWordInOperand(0));
                    }
                    if (def == nullptr || def->opcode() != spv::Op::OpTypeFloat) return 0;
                    return def->GetSingleWordInOperand(0);
                };

                std::vector<Instruction*> identityConversions;
                irContext->module()->ForEachInst(
                    [&](Instruction* inst) {
                        if (inst->opcode() != spv::Op::OpFConvert) return;
                        const Instruction* operandDef = defUseMgr->GetDef(inst->GetSingleWordInOperand(0));
                        if (operandDef == nullptr) return;
                        const Uint32 resultWidth = componentWidth(inst->type_id());
                        if (resultWidth == 0 || resultWidth != componentWidth(operandDef->type_id())) {
                            return;
                        }
                        identityConversions.push_back(inst);
                    },
                    /*run_on_debug_line_insts=*/false);

                for (Instruction* conversion : identityConversions) {
                    irContext->ReplaceAllUsesWith(conversion->result_id(),
                                                  conversion->GetSingleWordInOperand(0));
                    irContext->KillInst(conversion);
                }

                // --- 6. the capability -------------------------------------------------------
                std::vector<Instruction*> deadCapabilities;
                for (Instruction& capability : irContext->capabilities()) {
                    if (static_cast<spv::Capability>(capability.GetSingleWordInOperand(0)) ==
                        spv::Capability::Float64) {
                        deadCapabilities.push_back(&capability);
                    }
                }
                for (Instruction* capability : deadCapabilities) {
                    irContext->KillInst(capability);
                }

                // --- 7. merge what the rewrite made into a duplicate -------------------------
                // `double` and `float` are now the same declaration, and so is every vector,
                // matrix, pointer and function type spelled in terms of them. Walking the section
                // in declaration order and replacing each duplicate the moment it is found means
                // the later types are already canonical by the time they are keyed: SPIR-V forbids
                // a type from forward-referencing another, so every operand of the instruction
                // being looked at has been through this loop already.
                std::map<std::vector<Uint32>, Uint32> keptTypeByShape;
                for (Instruction* type = &*irContext->types_values_begin(); type != nullptr;) {
                    Instruction* next = type->NextNode();
                    if (!IsDuplicableType(type->opcode())) {
                        type = next;
                        continue;
                    }

                    std::vector<Uint32> shape{static_cast<Uint32>(type->opcode())};
                    for (Uint32 i = 0; i < type->NumInOperands(); ++i) {
                        const Operand& operand = type->GetInOperand(i);
                        shape.insert(shape.end(), operand.words.begin(), operand.words.end());
                    }

                    const auto kept = keptTypeByShape.find(shape);
                    if (kept == keptTypeByShape.end()) {
                        keptTypeByShape.emplace(std::move(shape), type->result_id());
                        type = next;
                        continue;
                    }

                    irContext->KillNamesAndDecorates(type->result_id());
                    irContext->ReplaceAllUsesWith(type->result_id(), kept->second);
                    irContext->KillInst(type);
                    type = next;
                }

                // --- 8. re-derive the layout of every block that held a 64-bit float -----------
                // See BlockRelayout for why this is a recomputation and not a preservation. Only
                // blocks that actually narrowed are touched: relaying out an untouched block would
                // be pure churn, and would risk disagreeing with glslang over a layout that was
                // already correct.
                for (Instruction& variable : irContext->types_values()) {
                    if (variable.opcode() != spv::Op::OpVariable) continue;
                    const auto storageClass =
                        static_cast<spv::StorageClass>(variable.GetSingleWordInOperand(0));
                    if (storageClass != spv::StorageClass::Uniform &&
                        storageClass != spv::StorageClass::StorageBuffer &&
                        storageClass != spv::StorageClass::PushConstant) {
                        continue;
                    }
                    const Instruction* pointerType = defUseMgr->GetDef(variable.type_id());
                    if (pointerType == nullptr) continue;

                    // An arrayed block (`uniform Blk { ... } blocks[4];`) is a pointer to an array
                    // of the struct; the layout lives on the struct either way.
                    const Instruction* blockType = defUseMgr->GetDef(pointerType->GetSingleWordInOperand(1));
                    while (blockType != nullptr && (blockType->opcode() == spv::Op::OpTypeArray ||
                                                    blockType->opcode() == spv::Op::OpTypeRuntimeArray)) {
                        blockType = defUseMgr->GetDef(blockType->GetSingleWordInOperand(0));
                    }
                    if (blockType == nullptr || blockType->opcode() != spv::Op::OpTypeStruct) continue;
                    if (wideTypeIds.count(blockType->result_id()) == 0) continue;

                    // A storage block packs std430, a uniform block std140. Before SPIR-V 1.3 a
                    // storage block was a Uniform-storage variable whose struct carried
                    // BufferBlock, so the decoration decides rather than the storage class alone.
                    Bool isStorageBlock = storageClass == spv::StorageClass::StorageBuffer;
                    for (const Instruction& annotation : irContext->annotations()) {
                        if (annotation.opcode() != spv::Op::OpDecorate) continue;
                        if (annotation.GetSingleWordInOperand(0) != blockType->result_id()) continue;
                        if (static_cast<spv::Decoration>(annotation.GetSingleWordInOperand(1)) ==
                            spv::Decoration::BufferBlock) {
                            isStorageBlock = true;
                        }
                    }

                    BlockRelayout relayout(irContext, /*std140=*/!isStorageBlock);
                    if (relayout.Measure(blockType->result_id()).size == 0) {
                        // A member shape the layout rules here do not describe. Leaving the block
                        // at its 64-bit offsets keeps the module valid for Vulkan; SPIRV-Cross will
                        // decline it for ESSL, which is the same outcome as before the demotion.
                        MGLOG_D("DemoteFloat64Pass: block %%%u contains a member this pass cannot lay "
                                "out; its 64-bit offsets are left in place",
                                blockType->result_id());
                    }
                }

                irContext->InvalidateAnalysesExceptFor(IRContext::kAnalysisNone);
                return Status::SuccessWithChange;
            }

            spvtools::Optimizer::PassToken DemoteFloat64Pass::CreateDemoteFloat64Pass() {
                return spvtools::Optimizer::PassToken(MakeUnique<DemoteFloat64Pass>());
            }
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
