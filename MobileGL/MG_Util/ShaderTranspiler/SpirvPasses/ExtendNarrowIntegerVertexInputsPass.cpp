// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/ExtendNarrowIntegerVertexInputsPass.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "ExtendNarrowIntegerVertexInputsPass.h"
#include "NarrowIntegerRuntimeWidths.h"

#include "spirv.hpp"
#include "source/opt/constants.h"
#include "source/opt/def_use_manager.h"
#include "source/opt/instruction.h"
#include "source/opt/ir_context.h"
#include "source/opt/module.h"
#include "source/opt/types.h"
#include "source/util/make_unique.h"

#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace MobileGL {
    namespace MG_Util {
        namespace ShaderTranspiler {
            namespace {
                using spvtools::opt::Instruction;
                using spvtools::opt::IRContext;
                using spvtools::opt::Operand;
                namespace analysis = spvtools::opt::analysis;

                constexpr Uint32 kMaxLocations = 32;

                // What one location's value needs: the width of the source data, and whether the
                // fetched value is sign-extended (true) or zero-extended (false) from it.
                struct Extension {
                    Uint32 bits = 0; // 0: leave the value alone
                    Bool sign = false;
                    // The width is read at run time (RuntimeWidths); `bits` is then only non-zero.
                    Bool runtime = false;
                };

                Extension ExtensionAt(const NarrowIntegerVertexInputs& inputs, Uint32 location) {
                    if (location >= kMaxLocations) return {};
                    const Uint32 bit = 1u << location;
                    if (inputs.zeroExtend8 & bit) return {8, false};
                    if (inputs.zeroExtend16 & bit) return {16, false};
                    if (inputs.signExtend8 & bit) return {8, true};
                    if (inputs.signExtend16 & bit) return {16, true};
                    return {};
                }

                // Every input below location 32 (a width bit exists for it), extended by its own rule.
                Extension RuntimeExtensionAt(Uint32 location, Bool unsignedInput) {
                    if (location >= kMaxLocations) return {};
                    return {32, unsignedInput, true};
                }

                Bool IsUnsignedInteger(const analysis::Type* type) {
                    if (const auto* vector = type->AsVector()) type = vector->element_type();
                    return !type->AsInteger()->IsSigned();
                }

                // True for a 32-bit integer scalar or vector: the only shapes a 32-bit integer
                // vertex attribute can feed, and the only ones the bit operations below accept.
                Bool IsInteger32ScalarOrVector(const analysis::Type* type) {
                    if (type == nullptr) return false;
                    if (const auto* vector = type->AsVector()) type = vector->element_type();
                    const auto* integer = type->AsInteger();
                    return integer != nullptr && integer->width() == 32;
                }

                // One input variable to correct, with every id the rewrite needs resolved up front.
                struct Target {
                    Instruction* variable = nullptr;
                    Uint32 pointeeTypeId = 0;
                    Uint32 elementTypeId = 0;
                    // 0 for a scalar or vector input; the element count of an array input.
                    Uint32 arrayLength = 0;
                    // One entry per element (one for a non-array input).
                    std::vector<Extension> extensions;
                    // Per element: the mask constant of a zero-extension, or the bit-count constant
                    // of a sign-extension; 0 where the element is left alone.
                    std::vector<Uint32> operandIds;
                    Uint32 privatePointerTypeId = 0;
                    Uint32 baseLocation = 0;
                    NarrowIntegerRuntimeWidths::ShiftType shiftType;
                    // Access chains rooted at the variable (chains of chains included), each with
                    // the Private pointer type its result takes once the variable is Private.
                    std::vector<std::pair<Instruction*, Uint32>> accessChains;
                };

                Bool IsDeclarationUse(const Instruction* user) {
                    switch (user->opcode()) {
                    case spv::Op::OpDecorate:
                    case spv::Op::OpDecorateId:
                    case spv::Op::OpDecorateString:
                    case spv::Op::OpName:
                    case spv::Op::OpEntryPoint:
                        return true;
                    default:
                        return false;
                    }
                }
            } // namespace

            spvtools::opt::Pass::Status ExtendNarrowIntegerVertexInputsPass::Process() {
                const Bool runtime = m_mode == Mode::RuntimeWidths;
                if (!runtime && !m_inputs.Any()) return Status::SuccessWithoutChange;

                auto* irContext = context();
                auto entryPoints = irContext->module()->entry_points();
                if (entryPoints.begin() == entryPoints.end()) return Status::SuccessWithoutChange;
                Instruction* entryPoint = &*entryPoints.begin();
                if (static_cast<spv::ExecutionModel>(entryPoint->GetSingleWordInOperand(0)) !=
                    spv::ExecutionModel::Vertex) {
                    return Status::SuccessWithoutChange;
                }
                // The Private copy is seeded in ONE entry point; a second one reading the same input
                // would see it unwritten. glslang never emits two, so refuse rather than half-fix.
                auto secondEntryPoint = entryPoints.begin();
                if (++secondEntryPoint != entryPoints.end()) return Status::Failure;

                auto* defUseMgr = irContext->get_def_use_mgr();
                auto* typeMgr = irContext->get_type_mgr();
                auto* constantMgr = irContext->get_constant_mgr();

                std::unordered_map<Uint32, Uint32> locationById;
                for (const Instruction& annotation : irContext->annotations()) {
                    if (annotation.opcode() != spv::Op::OpDecorate || annotation.NumInOperands() < 3) continue;
                    if (static_cast<spv::Decoration>(annotation.GetSingleWordInOperand(1)) == spv::Decoration::Location)
                        locationById[annotation.GetSingleWordInOperand(0)] = annotation.GetSingleWordInOperand(2);
                }

                std::vector<Target> targets;
                for (Instruction& inst : irContext->types_values()) {
                    if (inst.opcode() != spv::Op::OpVariable) continue;
                    if (static_cast<spv::StorageClass>(inst.GetSingleWordInOperand(0)) != spv::StorageClass::Input)
                        continue;
                    const auto location = locationById.find(inst.result_id());
                    if (location == locationById.end()) continue; // a built-in
                    const Instruction* pointerType = defUseMgr->GetDef(inst.type_id());
                    if (pointerType == nullptr) continue;

                    Target target;
                    target.variable = &inst;
                    target.pointeeTypeId = pointerType->GetSingleWordInOperand(1);
                    target.elementTypeId = target.pointeeTypeId;
                    Uint32 elementCount = 1;
                    const Instruction* pointee = defUseMgr->GetDef(target.pointeeTypeId);
                    if (pointee != nullptr && pointee->opcode() == spv::Op::OpTypeArray) {
                        const auto* length = constantMgr->FindDeclaredConstant(pointee->GetSingleWordInOperand(1));
                        if (length == nullptr || length->AsScalarConstant() == nullptr) continue;
                        target.elementTypeId = pointee->GetSingleWordInOperand(0);
                        target.arrayLength = static_cast<Uint32>(length->GetZeroExtendedValue());
                        elementCount = target.arrayLength;
                    }
                    const analysis::Type* elementType = typeMgr->GetType(target.elementTypeId);
                    if (!IsInteger32ScalarOrVector(elementType)) continue;
                    target.baseLocation = location->second;

                    Bool any = false;
                    for (Uint32 element = 0; element < elementCount; ++element) {
                        target.extensions.push_back(
                            runtime ? RuntimeExtensionAt(location->second + element, IsUnsignedInteger(elementType))
                                    : ExtensionAt(m_inputs, location->second + element));
                        any = any || target.extensions.back().bits != 0;
                    }
                    if (any) targets.push_back(std::move(target));
                }
                if (targets.empty()) return Status::SuccessWithoutChange;

                const Uint32 entryFunctionId = entryPoint->GetSingleWordInOperand(1);
                spvtools::opt::Function* entryFunction = nullptr;
                for (auto& function : *irContext->module()) {
                    if (function.result_id() == entryFunctionId) {
                        entryFunction = &function;
                        break;
                    }
                }
                if (entryFunction == nullptr || entryFunction->begin() == entryFunction->end()) {
                    return Status::Failure;
                }

                // Every type and constant the rewrite names is materialized BEFORE any variable is
                // added or moved: the types-and-values section is ordered, these lookups append to
                // it, and a variable may not forward-reference its type.
                const auto constantId = [&](Uint32 typeId, Uint32 scalarValue) -> Uint32 {
                    const analysis::Type* type = typeMgr->GetType(typeId);
                    if (type == nullptr) return 0;
                    const analysis::Constant* constant = nullptr;
                    if (const auto* vector = type->AsVector()) {
                        const analysis::Constant* component =
                            constantMgr->GetConstant(vector->element_type(), {scalarValue});
                        const Instruction* componentInst =
                            component ? constantMgr->GetDefiningInstruction(component) : nullptr;
                        if (componentInst == nullptr) return 0;
                        constant = constantMgr->GetConstant(
                            type, std::vector<Uint32>(vector->element_count(), componentInst->result_id()));
                    } else {
                        constant = constantMgr->GetConstant(type, {scalarValue});
                    }
                    const Instruction* inst =
                        constant ? constantMgr->GetDefiningInstruction(constant, typeId) : nullptr;
                    return inst ? inst->result_id() : 0;
                };
                NarrowIntegerRuntimeWidths::Ids runtimeIds;
                if (runtime && !NarrowIntegerRuntimeWidths::MaterializeTypes(irContext, runtimeIds))
                    return Status::Failure;
                Uint32 zeroOffsetId = 0;
                for (auto& target : targets) {
                    for (SizeT element = 0; element < target.extensions.size(); ++element) {
                        const Extension& extension = target.extensions[element];
                        Uint32 operandId = 0;
                        if (extension.runtime) {
                            // The location, which selects the element's bit in the two widths.
                            operandId = constantMgr->GetUIntConstId(target.baseLocation + static_cast<Uint32>(element));
                            if (operandId == 0) return Status::Failure;
                        } else if (extension.bits != 0) {
                            operandId = extension.sign ? constantMgr->GetUIntConstId(extension.bits)
                                                       : constantId(target.elementTypeId, (1u << extension.bits) - 1u);
                            if (operandId == 0) return Status::Failure;
                            if (extension.sign && zeroOffsetId == 0) {
                                zeroOffsetId = constantMgr->GetUIntConstId(0);
                                if (zeroOffsetId == 0) return Status::Failure;
                            }
                        }
                        target.operandIds.push_back(operandId);
                    }
                    if (runtime) {
                        target.shiftType = NarrowIntegerRuntimeWidths::MaterializeShiftType(irContext, runtimeIds,
                                                                                            target.elementTypeId);
                        if (target.shiftType.typeId == 0) return Status::Failure;
                    }
                    target.privatePointerTypeId =
                        typeMgr->FindPointerToType(target.pointeeTypeId, spv::StorageClass::Private);
                    if (target.privatePointerTypeId == 0) return Status::Failure;

                    // An access chain names its result's storage class, so every chain into the
                    // variable has to move to Private with it. Loads need nothing. Any other use -
                    // a pointer passed on, copied or stored - is refused before anything changes,
                    // rather than half-rewritten; glslang emits none of them for an input.
                    std::vector<Instruction*> pending{target.variable};
                    Bool unexpectedUse = false;
                    while (!pending.empty() && !unexpectedUse) {
                        Instruction* base = pending.back();
                        pending.pop_back();
                        // Collected first: the pointer-type lookups below may add instructions.
                        std::vector<Instruction*> users;
                        defUseMgr->ForEachUser(base, [&](Instruction* user) { users.push_back(user); });
                        for (Instruction* user : users) {
                            if (IsDeclarationUse(user) || user->opcode() == spv::Op::OpLoad) continue;
                            const Bool isChain = user->opcode() == spv::Op::OpAccessChain ||
                                                 user->opcode() == spv::Op::OpInBoundsAccessChain;
                            const Instruction* chainType =
                                isChain && user->GetSingleWordInOperand(0) == base->result_id()
                                    ? defUseMgr->GetDef(user->type_id())
                                    : nullptr;
                            const Uint32 privateChainTypeId =
                                chainType ? typeMgr->FindPointerToType(chainType->GetSingleWordInOperand(1),
                                                                       spv::StorageClass::Private)
                                          : 0;
                            if (privateChainTypeId == 0) {
                                unexpectedUse = true;
                                break;
                            }
                            target.accessChains.emplace_back(user, privateChainTypeId);
                            pending.push_back(user);
                        }
                    }
                    if (unexpectedUse) return Status::Failure;
                }

                // From SPIR-V 1.4 the interface lists every global the entry point uses, Private
                // included; before it, Input and Output only.
                const Bool interfaceListsPrivate = irContext->module()->version() >= 0x00010400u;

                auto& entryBlock = *entryFunction->begin();
                auto insertPoint = entryBlock.begin();
                while (insertPoint != entryBlock.end() && insertPoint->opcode() == spv::Op::OpVariable) {
                    ++insertPoint;
                }
                if (insertPoint == entryBlock.end()) return Status::Failure;

                // A TakeNextId failure (the id bound is exhausted) is reported once the rewrite is
                // over; the optimizer then discards the half-rewritten module.
                Bool outOfIds = false;
                const auto nextId = [&]() {
                    const Uint32 id = TakeNextId();
                    outOfIds = outOfIds || id == 0;
                    return id;
                };
                const auto insert = [&](spv::Op opcode, Uint32 typeId, std::initializer_list<Uint32> operands) {
                    const Uint32 resultId = typeId != 0 ? nextId() : 0;
                    Instruction::OperandList operandList;
                    for (const Uint32 id : operands)
                        operandList.push_back({SPV_OPERAND_TYPE_ID, {id}});
                    insertPoint = insertPoint.InsertBefore(
                        MakeUnique<Instruction>(irContext, opcode, typeId, resultId, operandList));
                    ++insertPoint;
                    return resultId;
                };
                const NarrowIntegerRuntimeWidths::Emitter emitter{irContext, &insertPoint, nextId};
                if (runtime) {
                    if (!NarrowIntegerRuntimeWidths::DeclareGlobals(irContext, runtimeIds, nextId))
                        return Status::Failure;
                    NarrowIntegerRuntimeWidths::LoadWidths(emitter, runtimeIds);
                }
                const auto extend = [&](const Target& target, SizeT element, Uint32 valueId) -> Uint32 {
                    const Extension& extension = target.extensions[element];
                    if (extension.bits == 0) return valueId;
                    if (extension.runtime) {
                        return NarrowIntegerRuntimeWidths::Extend(emitter, runtimeIds, target.elementTypeId,
                                                                  target.shiftType, target.operandIds[element],
                                                                  extension.sign, valueId);
                    }
                    if (!extension.sign) {
                        return insert(spv::Op::OpBitwiseAnd, target.elementTypeId,
                                      {valueId, target.operandIds[element]});
                    }
                    return insert(spv::Op::OpBitFieldSExtract, target.elementTypeId,
                                  {valueId, zeroOffsetId, target.operandIds[element]});
                };

                for (const auto& target : targets) {
                    Instruction* variable = target.variable;
                    const Uint32 privateId = variable->result_id();
                    const Uint32 inputId = nextId();
                    irContext->AddGlobalValue(MakeUnique<Instruction>(
                        irContext, spv::Op::OpVariable, variable->type_id(), inputId,
                        std::initializer_list<Operand>{
                            {SPV_OPERAND_TYPE_STORAGE_CLASS, {static_cast<Uint32>(spv::StorageClass::Input)}}}));

                    // Every decoration describes the interface, so all of them follow the Input.
                    for (auto& annotation : irContext->annotations()) {
                        const spv::Op op = annotation.opcode();
                        if ((op == spv::Op::OpDecorate || op == spv::Op::OpDecorateId ||
                             op == spv::Op::OpDecorateString) &&
                            annotation.GetSingleWordInOperand(0) == privateId) {
                            annotation.SetInOperand(0, {inputId});
                        }
                    }

                    // The original becomes the Private copy every existing access keeps reading. It
                    // moves to the end of the section because its new pointer type may be newer.
                    variable->SetResultType(target.privatePointerTypeId);
                    variable->SetInOperand(0, {static_cast<Uint32>(spv::StorageClass::Private)});
                    variable->RemoveFromList();
                    irContext->AddGlobalValue(std::unique_ptr<Instruction>(variable));
                    for (const auto& [chain, privateChainTypeId] : target.accessChains)
                        chain->SetResultType(privateChainTypeId);

                    std::vector<Operand> interfaceOperands;
                    for (Uint32 i = 0; i < entryPoint->NumInOperands(); ++i) {
                        const Operand& operand = entryPoint->GetInOperand(i);
                        if (i >= 3 && !interfaceListsPrivate && entryPoint->GetSingleWordInOperand(i) == privateId) {
                            interfaceOperands.push_back({SPV_OPERAND_TYPE_ID, {inputId}});
                            continue;
                        }
                        interfaceOperands.push_back(operand);
                    }
                    if (interfaceListsPrivate) interfaceOperands.push_back({SPV_OPERAND_TYPE_ID, {inputId}});
                    entryPoint->SetInOperands(std::move(interfaceOperands));

                    const Uint32 loadedId = insert(spv::Op::OpLoad, target.pointeeTypeId, {inputId});
                    Uint32 correctedId = 0;
                    if (target.arrayLength == 0) {
                        correctedId = extend(target, 0, loadedId);
                    } else {
                        std::vector<Uint32> elementIds;
                        for (Uint32 element = 0; element < target.arrayLength; ++element) {
                            const Uint32 elementId = nextId();
                            insertPoint = insertPoint.InsertBefore(MakeUnique<Instruction>(
                                irContext, spv::Op::OpCompositeExtract, target.elementTypeId, elementId,
                                Instruction::OperandList{{SPV_OPERAND_TYPE_ID, {loadedId}},
                                                         {SPV_OPERAND_TYPE_LITERAL_INTEGER, {element}}}));
                            ++insertPoint;
                            elementIds.push_back(extend(target, element, elementId));
                        }
                        correctedId = nextId();
                        Instruction::OperandList constituents;
                        for (const Uint32 id : elementIds)
                            constituents.push_back({SPV_OPERAND_TYPE_ID, {id}});
                        insertPoint = insertPoint.InsertBefore(MakeUnique<Instruction>(
                            irContext, spv::Op::OpCompositeConstruct, target.pointeeTypeId, correctedId, constituents));
                        ++insertPoint;
                    }
                    insert(spv::Op::OpStore, 0, {privateId, correctedId});
                }
                if (runtime && interfaceListsPrivate) {
                    entryPoint->AddOperand({SPV_OPERAND_TYPE_ID, {runtimeIds.widths8Id}});
                    entryPoint->AddOperand({SPV_OPERAND_TYPE_ID, {runtimeIds.widths16Id}});
                }

                if (outOfIds) return Status::Failure;
                irContext->InvalidateAnalysesExceptFor(IRContext::kAnalysisNone);
                if (m_outChanged != nullptr) *m_outChanged = true;
                return Status::SuccessWithChange;
            }

            spvtools::Optimizer::PassToken ExtendNarrowIntegerVertexInputsPass::
                CreateExtendNarrowIntegerVertexInputsPass(const NarrowIntegerVertexInputs& inputs) {
                return spvtools::Optimizer::PassToken(MakeUnique<ExtendNarrowIntegerVertexInputsPass>(inputs));
            }

            spvtools::Optimizer::PassToken ExtendNarrowIntegerVertexInputsPass::CreateRuntimeWidthsPass(
                Bool* outChanged) {
                return spvtools::Optimizer::PassToken(MakeUnique<ExtendNarrowIntegerVertexInputsPass>(outChanged));
            }
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
