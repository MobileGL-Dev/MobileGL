// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/DeferGeometryEmitsPass.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "DeferGeometryEmitsPass.h"

#include "spirv.hpp"
#include "source/opt/basic_block.h"
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

#include <algorithm>
#include <memory>
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

                // Scalars of snapshot per invocation above which the pass declines. A geometry
                // stage's own output budget (GL_MAX_GEOMETRY_TOTAL_OUTPUT_COMPONENTS) is 1024 on
                // every ES 3.2 driver measured, so a legal shader stays well inside this; the cap
                // only guards against a pathological one turning into a huge private array.
                constexpr uint32_t kMaxSnapshotScalars = 8192;

                Operand Id(uint32_t id) {
                    return {SPV_OPERAND_TYPE_ID, {id}};
                }

                std::unique_ptr<Instruction> Make(IRContext* context, spv::Op op, uint32_t typeId, uint32_t resultId,
                                                  OperandList operands) {
                    return spvtools::MakeUnique<Instruction>(context, op, typeId, resultId, operands);
                }

                // The value of an OpConstant integer, or nullopt for anything else (a spec
                // constant included: a length that can change after the pass cannot size a
                // snapshot).
                Optional<uint32_t> ConstantValue(IRContext* context, uint32_t id) {
                    const Instruction* def = context->get_def_use_mgr()->GetDef(id);
                    if (def == nullptr || def->opcode() != spv::Op::OpConstant || def->NumInOperands() < 1)
                        return std::nullopt;
                    return def->GetSingleWordInOperand(0);
                }

                // Scalar components a value of this type holds, or nullopt when it holds anything
                // the snapshot cannot copy by value (a pointer, an opaque handle, a runtime array).
                Optional<uint32_t> ScalarCount(IRContext* context, uint32_t typeId) {
                    const Instruction* type = context->get_def_use_mgr()->GetDef(typeId);
                    if (type == nullptr) return std::nullopt;
                    switch (type->opcode()) {
                    case spv::Op::OpTypeInt:
                    case spv::Op::OpTypeFloat:
                    case spv::Op::OpTypeBool:
                        return 1u;
                    case spv::Op::OpTypeVector:
                    case spv::Op::OpTypeMatrix: {
                        const auto inner = ScalarCount(context, type->GetSingleWordInOperand(0));
                        if (!inner) return std::nullopt;
                        return *inner * type->GetSingleWordInOperand(1);
                    }
                    case spv::Op::OpTypeArray: {
                        const auto inner = ScalarCount(context, type->GetSingleWordInOperand(0));
                        const auto length = ConstantValue(context, type->GetSingleWordInOperand(1));
                        if (!inner || !length) return std::nullopt;
                        return *inner * *length;
                    }
                    case spv::Op::OpTypeStruct: {
                        uint32_t total = 0;
                        for (uint32_t i = 0; i < type->NumInOperands(); ++i) {
                            const auto inner = ScalarCount(context, type->GetSingleWordInOperand(i));
                            if (!inner) return std::nullopt;
                            total += *inner;
                        }
                        return total;
                    }
                    default:
                        return std::nullopt;
                    }
                }

                spv::StorageClass PointerStorageClass(IRContext* context, uint32_t pointerId) {
                    const Instruction* pointer = context->get_def_use_mgr()->GetDef(pointerId);
                    if (pointer == nullptr || pointer->type_id() == 0) return spv::StorageClass::Max;
                    const Instruction* type = context->get_def_use_mgr()->GetDef(pointer->type_id());
                    if (type == nullptr || type->opcode() != spv::Op::OpTypePointer) return spv::StorageClass::Max;
                    return static_cast<spv::StorageClass>(type->GetSingleWordInOperand(0));
                }

                // Every way a shader writes memory another invocation - or the application - can
                // see: a store into a buffer or image block, an image store, any atomic.
                Bool IsMemoryWrite(IRContext* context, const Instruction& inst) {
                    switch (inst.opcode()) {
                    case spv::Op::OpImageWrite:
                    case spv::Op::OpAtomicStore:
                    case spv::Op::OpAtomicExchange:
                    case spv::Op::OpAtomicCompareExchange:
                    case spv::Op::OpAtomicCompareExchangeWeak:
                    case spv::Op::OpAtomicIIncrement:
                    case spv::Op::OpAtomicIDecrement:
                    case spv::Op::OpAtomicIAdd:
                    case spv::Op::OpAtomicISub:
                    case spv::Op::OpAtomicSMin:
                    case spv::Op::OpAtomicUMin:
                    case spv::Op::OpAtomicSMax:
                    case spv::Op::OpAtomicUMax:
                    case spv::Op::OpAtomicAnd:
                    case spv::Op::OpAtomicOr:
                    case spv::Op::OpAtomicXor:
                    case spv::Op::OpAtomicFlagTestAndSet:
                    case spv::Op::OpAtomicFlagClear:
                    case spv::Op::OpAtomicFAddEXT:
                    case spv::Op::OpAtomicFMinEXT:
                    case spv::Op::OpAtomicFMaxEXT:
                        return true;
                    case spv::Op::OpStore: {
                        const spv::StorageClass storage = PointerStorageClass(context, inst.GetSingleWordInOperand(0));
                        return storage == spv::StorageClass::StorageBuffer || storage == spv::StorageClass::Uniform ||
                               storage == spv::StorageClass::Image ||
                               storage == spv::StorageClass::PhysicalStorageBuffer;
                    }
                    default:
                        return false;
                    }
                }

                // Whether anything stores through this pointer, directly or through an access
                // chain into it. A pointer handed to a function call counts as stored: the callee
                // is not followed, and an output passed by pointer is one the shader means to set.
                Bool IsStoredThrough(IRContext* context, Instruction* pointer) {
                    Bool stored = false;
                    context->get_def_use_mgr()->WhileEachUser(pointer, [&](Instruction* user) {
                        switch (user->opcode()) {
                        case spv::Op::OpStore:
                            if (user->GetSingleWordInOperand(0) == pointer->result_id()) stored = true;
                            break;
                        case spv::Op::OpCopyMemory:
                        case spv::Op::OpCopyMemorySized:
                            if (user->GetSingleWordInOperand(0) == pointer->result_id()) stored = true;
                            break;
                        case spv::Op::OpFunctionCall:
                            stored = true;
                            break;
                        case spv::Op::OpAccessChain:
                        case spv::Op::OpInBoundsAccessChain:
                            stored = IsStoredThrough(context, user);
                            break;
                        default:
                            break;
                        }
                        return !stored;
                    });
                    return stored;
                }

                // One output value the snapshot carries: a whole output variable, or one member
                // of an output block.
                struct Slot {
                    uint32_t variableId = 0;
                    Int64 member = -1;
                    uint32_t valueTypeId = 0;
                    // Filled once the globals exist.
                    uint32_t memberIndexId = 0;
                    uint32_t outputPointerTypeId = 0;
                    uint32_t snapshotVariableId = 0;
                    uint32_t snapshotElementPointerTypeId = 0;
                };

                void ReplaceName(IRContext* context, uint32_t id, const char* name) {
                    for (auto& debugInst : context->debugs2()) {
                        if (debugInst.opcode() == spv::Op::OpName && debugInst.GetSingleWordInOperand(0) == id) {
                            debugInst.SetInOperand(
                                1, spvtools::utils::MakeVector<spvtools::opt::Operand::OperandData>(name));
                            return;
                        }
                    }
                    context->AddDebug2Inst(
                        Make(context, spv::Op::OpName, 0, 0,
                             {Id(id), {SPV_OPERAND_TYPE_LITERAL_STRING, spvtools::utils::MakeVector(name)}}));
                }
            } // namespace

            spvtools::opt::Pass::Status DeferGeometryEmitsPass::Process() {
                IRContext* irContext = context();
                if (m_outDeferred != nullptr) *m_outDeferred = false;

                // One geometry entry point, sized by max_vertices.
                Instruction* entryPoint = nullptr;
                for (Instruction& candidate : irContext->module()->entry_points()) {
                    if (entryPoint != nullptr) return Status::SuccessWithoutChange;
                    entryPoint = &candidate;
                }
                if (entryPoint == nullptr || static_cast<spv::ExecutionModel>(entryPoint->GetSingleWordInOperand(0)) !=
                                                 spv::ExecutionModel::Geometry) {
                    return Status::SuccessWithoutChange;
                }
                const uint32_t bodyFunctionId = entryPoint->GetSingleWordInOperand(1);
                uint32_t maxVertices = 0;
                for (const Instruction& mode : irContext->module()->execution_modes()) {
                    if (mode.opcode() == spv::Op::OpExecutionMode && mode.GetSingleWordInOperand(0) == bodyFunctionId &&
                        static_cast<spv::ExecutionMode>(mode.GetSingleWordInOperand(1)) ==
                            spv::ExecutionMode::OutputVertices) {
                        maxVertices = mode.GetSingleWordInOperand(2);
                    }
                }
                if (maxVertices == 0) return Status::SuccessWithoutChange;

                std::vector<Instruction*> emits;
                std::vector<Instruction*> cuts;
                Bool writesMemory = false;
                for (Function& function : *irContext->module()) {
                    Bool streams = false;
                    function.ForEachInst([&](Instruction* inst) {
                        switch (inst->opcode()) {
                        case spv::Op::OpEmitVertex:
                            emits.push_back(inst);
                            break;
                        case spv::Op::OpEndPrimitive:
                            cuts.push_back(inst);
                            break;
                        case spv::Op::OpEmitStreamVertex:
                        case spv::Op::OpEndStreamPrimitive:
                            streams = true;
                            break;
                        default:
                            if (!writesMemory && IsMemoryWrite(irContext, *inst)) writesMemory = true;
                            break;
                        }
                    });
                    if (streams) return Status::SuccessWithoutChange;
                }
                if (emits.empty() || !writesMemory) return Status::SuccessWithoutChange;

                // What the snapshot carries.
                auto* defUse = irContext->get_def_use_mgr();
                auto* decorations = irContext->get_decoration_mgr();
                std::vector<Slot> slots;
                uint64_t snapshotScalars = 0;
                for (Instruction& global : irContext->types_values()) {
                    if (global.opcode() != spv::Op::OpVariable ||
                        static_cast<spv::StorageClass>(global.GetSingleWordInOperand(0)) != spv::StorageClass::Output) {
                        continue;
                    }
                    const Instruction* pointerType = defUse->GetDef(global.type_id());
                    const uint32_t valueTypeId = pointerType->GetSingleWordInOperand(1);
                    const Instruction* valueType = defUse->GetDef(valueTypeId);
                    const Bool block = valueType->opcode() == spv::Op::OpTypeStruct &&
                                       decorations->HasDecoration(valueTypeId, uint32_t(spv::Decoration::Block));
                    if (!block) {
                        if (!IsStoredThrough(irContext, &global)) continue;
                        const auto scalars = ScalarCount(irContext, valueTypeId);
                        if (!scalars) return Status::SuccessWithoutChange;
                        snapshotScalars += uint64_t(*scalars) * maxVertices;
                        slots.push_back({global.result_id(), -1, valueTypeId});
                        continue;
                    }
                    // A block: per written member, so an unwritten builtin member stays inactive.
                    std::vector<Bool> written(valueType->NumInOperands(), false);
                    Bool dynamicMember = false;
                    defUse->ForEachUser(&global, [&](Instruction* user) {
                        if (user->opcode() == spv::Op::OpStore &&
                            user->GetSingleWordInOperand(0) == global.result_id()) {
                            std::fill(written.begin(), written.end(), true);
                            return;
                        }
                        if (user->opcode() == spv::Op::OpFunctionCall) {
                            std::fill(written.begin(), written.end(), true);
                            return;
                        }
                        if (user->opcode() != spv::Op::OpAccessChain &&
                            user->opcode() != spv::Op::OpInBoundsAccessChain)
                            return;
                        if (user->NumInOperands() < 2) return;
                        const auto member = ConstantValue(irContext, user->GetSingleWordInOperand(1));
                        if (!member || *member >= written.size()) {
                            dynamicMember = true;
                            return;
                        }
                        if (IsStoredThrough(irContext, user)) written[*member] = true;
                    });
                    if (dynamicMember) return Status::SuccessWithoutChange;
                    for (uint32_t member = 0; member < written.size(); ++member) {
                        if (!written[member]) continue;
                        const uint32_t memberTypeId = valueType->GetSingleWordInOperand(member);
                        const auto scalars = ScalarCount(irContext, memberTypeId);
                        if (!scalars) return Status::SuccessWithoutChange;
                        snapshotScalars += uint64_t(*scalars) * maxVertices;
                        slots.push_back({global.result_id(), Int64(member), memberTypeId});
                    }
                }
                if (snapshotScalars > kMaxSnapshotScalars) return Status::SuccessWithoutChange;

                // ---- Types, constants and Private globals, all before any code moves ----------
                auto* typeMgr = irContext->get_type_mgr();
                auto* constMgr = irContext->get_constant_mgr();
                const uint32_t uintTypeId = typeMgr->GetUIntTypeId();
                const uint32_t boolTypeId = typeMgr->GetBoolTypeId();
                const uint32_t voidTypeId = typeMgr->GetVoidTypeId();
                const uint32_t zeroId = constMgr->GetUIntConstId(0);
                const uint32_t oneId = constMgr->GetUIntConstId(1);
                const uint32_t lengthId = constMgr->GetUIntConstId(maxVertices);
                const uint32_t lastId = constMgr->GetUIntConstId(maxVertices - 1);
                const uint32_t cutLengthId = constMgr->GetUIntConstId(maxVertices + 1);
                const uint32_t privateUintPointerId =
                    typeMgr->FindPointerToType(uintTypeId, spv::StorageClass::Private);
                const uint32_t functionUintPointerId =
                    typeMgr->FindPointerToType(uintTypeId, spv::StorageClass::Function);
                for (Slot& slot : slots) {
                    slot.snapshotElementPointerTypeId =
                        typeMgr->FindPointerToType(slot.valueTypeId, spv::StorageClass::Private);
                    if (slot.member >= 0) {
                        slot.memberIndexId = constMgr->GetUIntConstId(uint32_t(slot.member));
                        slot.outputPointerTypeId =
                            typeMgr->FindPointerToType(slot.valueTypeId, spv::StorageClass::Output);
                    }
                }
                const uint32_t bodyFunctionTypeId =
                    irContext->GetFunction(bodyFunctionId)->DefInst().GetSingleWordInOperand(1);

                // Hand-built from here on; analyses are rebuilt once at the end.
                auto newPrivateArray = [&](uint32_t elementTypeId, uint32_t lengthConstantId) {
                    const uint32_t arrayTypeId = irContext->TakeNextId();
                    irContext->AddType(Make(irContext, spv::Op::OpTypeArray, 0, arrayTypeId,
                                            {Id(elementTypeId), Id(lengthConstantId)}));
                    const uint32_t pointerTypeId = irContext->TakeNextId();
                    irContext->AddType(Make(
                        irContext, spv::Op::OpTypePointer, 0, pointerTypeId,
                        {{SPV_OPERAND_TYPE_STORAGE_CLASS, {uint32_t(spv::StorageClass::Private)}}, Id(arrayTypeId)}));
                    const uint32_t variableId = irContext->TakeNextId();
                    irContext->AddGlobalValue(
                        Make(irContext, spv::Op::OpVariable, pointerTypeId, variableId,
                             {{SPV_OPERAND_TYPE_STORAGE_CLASS, {uint32_t(spv::StorageClass::Private)}}}));
                    return variableId;
                };
                for (Slot& slot : slots)
                    slot.snapshotVariableId = newPrivateArray(slot.valueTypeId, lengthId);
                // One slot past max_vertices: where an EndPrimitive with no vertex before it lands.
                const uint32_t cutVariableId = newPrivateArray(uintTypeId, cutLengthId);
                const uint32_t countVariableId = irContext->TakeNextId();
                irContext->AddGlobalValue(
                    Make(irContext, spv::Op::OpVariable, privateUintPointerId, countVariableId,
                         {{SPV_OPERAND_TYPE_STORAGE_CLASS, {uint32_t(spv::StorageClass::Private)}}}));
                ReplaceName(irContext, countVariableId, "mg_deferredVertexCount");
                ReplaceName(irContext, cutVariableId, "mg_deferredStripEnd");

                irContext->InvalidateAnalysesExceptFor(IRContext::kAnalysisNone);

                // Emits a value-producing instruction before |at| and returns its id.
                auto before = [&](Instruction* at, spv::Op op, uint32_t typeId, OperandList operands) {
                    const uint32_t id = typeId != 0 ? irContext->TakeNextId() : 0;
                    at->InsertBefore(Make(irContext, op, typeId, id, std::move(operands)));
                    return id;
                };
                auto outputPointer = [&](Instruction* at, const Slot& slot) {
                    if (slot.member < 0) return slot.variableId;
                    return before(at, spv::Op::OpAccessChain, slot.outputPointerTypeId,
                                  {Id(slot.variableId), Id(slot.memberIndexId)});
                };

                // ---- Each EmitVertex records; each EndPrimitive marks ---------------------------
                for (Instruction* emit : emits) {
                    const uint32_t count = before(emit, spv::Op::OpLoad, uintTypeId, {Id(countVariableId)});
                    const uint32_t inRange = before(emit, spv::Op::OpULessThan, boolTypeId, {Id(count), Id(lengthId)});
                    const uint32_t index =
                        before(emit, spv::Op::OpSelect, uintTypeId, {Id(inRange), Id(count), Id(lastId)});
                    for (const Slot& slot : slots) {
                        const uint32_t source = outputPointer(emit, slot);
                        const uint32_t value = before(emit, spv::Op::OpLoad, slot.valueTypeId, {Id(source)});
                        const uint32_t target = before(emit, spv::Op::OpAccessChain, slot.snapshotElementPointerTypeId,
                                                       {Id(slot.snapshotVariableId), Id(index)});
                        before(emit, spv::Op::OpStore, 0, {Id(target), Id(value)});
                    }
                    const uint32_t cut =
                        before(emit, spv::Op::OpAccessChain, privateUintPointerId, {Id(cutVariableId), Id(index)});
                    before(emit, spv::Op::OpStore, 0, {Id(cut), Id(zeroId)});
                    const uint32_t next = before(emit, spv::Op::OpIAdd, uintTypeId, {Id(count), Id(oneId)});
                    before(emit, spv::Op::OpStore, 0, {Id(countVariableId), Id(next)});
                }
                for (Instruction* end : cuts) {
                    const uint32_t count = before(end, spv::Op::OpLoad, uintTypeId, {Id(countVariableId)});
                    const uint32_t any = before(end, spv::Op::OpINotEqual, boolTypeId, {Id(count), Id(zeroId)});
                    const uint32_t previous = before(end, spv::Op::OpISub, uintTypeId, {Id(count), Id(oneId)});
                    const uint32_t inRange =
                        before(end, spv::Op::OpULessThan, boolTypeId, {Id(previous), Id(lengthId)});
                    const uint32_t clamped =
                        before(end, spv::Op::OpSelect, uintTypeId, {Id(inRange), Id(previous), Id(lastId)});
                    const uint32_t index =
                        before(end, spv::Op::OpSelect, uintTypeId, {Id(any), Id(clamped), Id(lengthId)});
                    const uint32_t cut =
                        before(end, spv::Op::OpAccessChain, privateUintPointerId, {Id(cutVariableId), Id(index)});
                    before(end, spv::Op::OpStore, 0, {Id(cut), Id(oneId)});
                }
                for (Instruction* inst : emits)
                    irContext->KillInst(inst);
                for (Instruction* inst : cuts)
                    irContext->KillInst(inst);

                // ---- The new entry point: run the body, then replay -----------------------------
                const uint32_t entryId = irContext->TakeNextId();
                auto function = spvtools::MakeUnique<Function>(
                    Make(irContext, spv::Op::OpFunction, voidTypeId, entryId,
                         {{SPV_OPERAND_TYPE_FUNCTION_CONTROL, {0}}, Id(bodyFunctionTypeId)}));
                Function* entry = function.get();
                const uint32_t startLabel = irContext->TakeNextId();
                const uint32_t headerLabel = irContext->TakeNextId();
                const uint32_t checkLabel = irContext->TakeNextId();
                const uint32_t replayLabel = irContext->TakeNextId();
                const uint32_t endStripLabel = irContext->TakeNextId();
                const uint32_t afterStripLabel = irContext->TakeNextId();
                const uint32_t continueLabel = irContext->TakeNextId();
                const uint32_t mergeLabel = irContext->TakeNextId();
                auto newBlock = [&](uint32_t label) {
                    auto block = spvtools::MakeUnique<BasicBlock>(Make(irContext, spv::Op::OpLabel, 0, label, {}));
                    block->SetParent(entry);
                    return block;
                };
                auto add = [&](BasicBlock& block, spv::Op op, uint32_t typeId, OperandList operands) {
                    const uint32_t id = typeId != 0 ? irContext->TakeNextId() : 0;
                    block.AddInstruction(Make(irContext, op, typeId, id, std::move(operands)));
                    return id;
                };

                auto start = newBlock(startLabel);
                const uint32_t iterator =
                    add(*start, spv::Op::OpVariable, functionUintPointerId,
                        {{SPV_OPERAND_TYPE_STORAGE_CLASS, {uint32_t(spv::StorageClass::Function)}}});
                add(*start, spv::Op::OpStore, 0, {Id(countVariableId), Id(zeroId)});
                add(*start, spv::Op::OpFunctionCall, voidTypeId, {Id(bodyFunctionId)});
                const uint32_t recorded = add(*start, spv::Op::OpLoad, uintTypeId, {Id(countVariableId)});
                const uint32_t fits = add(*start, spv::Op::OpULessThan, boolTypeId, {Id(recorded), Id(lengthId)});
                const uint32_t limit =
                    add(*start, spv::Op::OpSelect, uintTypeId, {Id(fits), Id(recorded), Id(lengthId)});
                add(*start, spv::Op::OpStore, 0, {Id(iterator), Id(zeroId)});
                add(*start, spv::Op::OpBranch, 0, {Id(headerLabel)});

                auto header = newBlock(headerLabel);
                add(*header, spv::Op::OpLoopMerge, 0,
                    {Id(mergeLabel), Id(continueLabel), {SPV_OPERAND_TYPE_LOOP_CONTROL, {0}}});
                add(*header, spv::Op::OpBranch, 0, {Id(checkLabel)});

                auto check = newBlock(checkLabel);
                const uint32_t vertex = add(*check, spv::Op::OpLoad, uintTypeId, {Id(iterator)});
                const uint32_t more = add(*check, spv::Op::OpULessThan, boolTypeId, {Id(vertex), Id(limit)});
                add(*check, spv::Op::OpBranchConditional, 0, {Id(more), Id(replayLabel), Id(mergeLabel)});

                auto replay = newBlock(replayLabel);
                for (const Slot& slot : slots) {
                    const uint32_t source = add(*replay, spv::Op::OpAccessChain, slot.snapshotElementPointerTypeId,
                                                {Id(slot.snapshotVariableId), Id(vertex)});
                    const uint32_t value = add(*replay, spv::Op::OpLoad, slot.valueTypeId, {Id(source)});
                    uint32_t target = slot.variableId;
                    if (slot.member >= 0) {
                        target = add(*replay, spv::Op::OpAccessChain, slot.outputPointerTypeId,
                                     {Id(slot.variableId), Id(slot.memberIndexId)});
                    }
                    add(*replay, spv::Op::OpStore, 0, {Id(target), Id(value)});
                }
                add(*replay, spv::Op::OpEmitVertex, 0, {});
                const uint32_t cutPointer =
                    add(*replay, spv::Op::OpAccessChain, privateUintPointerId, {Id(cutVariableId), Id(vertex)});
                const uint32_t cutValue = add(*replay, spv::Op::OpLoad, uintTypeId, {Id(cutPointer)});
                const uint32_t endsStrip = add(*replay, spv::Op::OpINotEqual, boolTypeId, {Id(cutValue), Id(zeroId)});
                add(*replay, spv::Op::OpSelectionMerge, 0,
                    {Id(afterStripLabel), {SPV_OPERAND_TYPE_SELECTION_CONTROL, {0}}});
                add(*replay, spv::Op::OpBranchConditional, 0, {Id(endsStrip), Id(endStripLabel), Id(afterStripLabel)});

                auto endStrip = newBlock(endStripLabel);
                add(*endStrip, spv::Op::OpEndPrimitive, 0, {});
                add(*endStrip, spv::Op::OpBranch, 0, {Id(afterStripLabel)});

                auto afterStrip = newBlock(afterStripLabel);
                add(*afterStrip, spv::Op::OpBranch, 0, {Id(continueLabel)});

                auto continueBlock = newBlock(continueLabel);
                const uint32_t current = add(*continueBlock, spv::Op::OpLoad, uintTypeId, {Id(iterator)});
                const uint32_t next = add(*continueBlock, spv::Op::OpIAdd, uintTypeId, {Id(current), Id(oneId)});
                add(*continueBlock, spv::Op::OpStore, 0, {Id(iterator), Id(next)});
                add(*continueBlock, spv::Op::OpBranch, 0, {Id(headerLabel)});

                auto merge = newBlock(mergeLabel);
                add(*merge, spv::Op::OpReturn, 0, {});

                entry->AddBasicBlock(std::move(start));
                entry->AddBasicBlock(std::move(header));
                entry->AddBasicBlock(std::move(check));
                entry->AddBasicBlock(std::move(replay));
                entry->AddBasicBlock(std::move(endStrip));
                entry->AddBasicBlock(std::move(afterStrip));
                entry->AddBasicBlock(std::move(continueBlock));
                entry->AddBasicBlock(std::move(merge));
                entry->SetFunctionEnd(Make(irContext, spv::Op::OpFunctionEnd, 0, 0, {}));
                irContext->AddFunction(std::move(function));

                // The entry point and its execution modes now name the new function.
                entryPoint->SetInOperand(1, {entryId});
                for (Instruction& mode : irContext->module()->execution_modes()) {
                    if (mode.GetSingleWordInOperand(0) == bodyFunctionId) mode.SetInOperand(0, {entryId});
                }
                ReplaceName(irContext, bodyFunctionId, "mg_geometryBody");
                ReplaceName(irContext, entryId, "main");

                irContext->InvalidateAnalysesExceptFor(IRContext::kAnalysisNone);
                if (m_outDeferred != nullptr) *m_outDeferred = true;
                return Status::SuccessWithChange;
            }

            spvtools::Optimizer::PassToken DeferGeometryEmitsPass::CreateDeferGeometryEmitsPass(Bool* outDeferred) {
                return spvtools::Optimizer::PassToken(spvtools::MakeUnique<DeferGeometryEmitsPass>(outDeferred));
            }
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
