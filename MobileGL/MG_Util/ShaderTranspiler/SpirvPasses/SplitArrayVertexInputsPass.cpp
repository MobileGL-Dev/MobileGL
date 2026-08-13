// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/SplitArrayVertexInputsPass.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "SplitArrayVertexInputsPass.h"

#include "spirv.hpp"
#include "source/opt/constants.h"
#include "source/opt/def_use_manager.h"
#include "source/opt/instruction.h"
#include "source/opt/ir_context.h"
#include "source/opt/module.h"
#include "source/opt/types.h"
#include "source/util/make_unique.h"

#include <memory>
#include <unordered_set>
#include <vector>

namespace MobileGL {
    namespace MG_Util {
        namespace ShaderTranspiler {
            namespace {
                using spvtools::opt::IRContext;
                using spvtools::opt::Instruction;
                using spvtools::opt::Operand;
                namespace analysis = spvtools::opt::analysis;

                // How many attribute locations one element of the array consumes. GL 4.6 core
                // 11.1.1: a scalar or vector of up to four 32-bit components takes one; a
                // double-precision vector wider than two takes two; a matrix takes one per
                // column. Zero means "this pass will not touch it" - the module keeps its array
                // input and SPIRV-Cross will say so, which is a better outcome than a silently
                // mis-located split.
                Uint32 LocationsPerElement(const analysis::Type* type) {
                    if (type == nullptr) return 0;
                    // Any scalar takes one location, a 64-bit one included.
                    if (type->AsFloat() != nullptr || type->AsInteger() != nullptr ||
                        type->AsBool() != nullptr) {
                        return 1u;
                    }
                    if (const auto* vector = type->AsVector()) {
                        const auto* element = vector->element_type();
                        const auto* elementFloat = element->AsFloat();
                        const Bool is64Bit = elementFloat != nullptr && elementFloat->width() == 64;
                        if (element->AsFloat() == nullptr && element->AsInteger() == nullptr &&
                            element->AsBool() == nullptr) {
                            return 0;
                        }
                        return (is64Bit && vector->element_count() > 2) ? 2u : 1u;
                    }
                    // Matrices, structs, images and nested arrays are left alone deliberately:
                    // an array of them is vanishingly rare as a vertex input and each carries its
                    // own location-assignment rule, so getting one wrong would corrupt every
                    // attribute after it rather than fail loudly.
                    return 0;
                }

                // The Location a variable is decorated with, or `false` when it carries none.
                // A vertex input without an explicit location cannot be split: the split has to
                // name base+i, and inventing a base would collide with whatever the linker
                // assigned.
                Bool FindLocationDecoration(IRContext& irContext, Uint32 variableId, Uint32& outLocation) {
                    for (auto& annotation : irContext.annotations()) {
                        if (annotation.opcode() != spv::Op::OpDecorate) continue;
                        if (annotation.GetSingleWordInOperand(0) != variableId) continue;
                        if (static_cast<spv::Decoration>(annotation.GetSingleWordInOperand(1)) !=
                            spv::Decoration::Location) {
                            continue;
                        }
                        outLocation = annotation.GetSingleWordInOperand(2);
                        return true;
                    }
                    return false;
                }
            } // namespace

            spvtools::opt::Pass::Status SplitArrayVertexInputsPass::Process() {
                auto* irContext = context();
                auto entryPoints = irContext->module()->entry_points();
                if (entryPoints.begin() == entryPoints.end()) return Status::SuccessWithoutChange;

                Instruction* entryPoint = &*entryPoints.begin();
                if (static_cast<spv::ExecutionModel>(entryPoint->GetSingleWordInOperand(0)) !=
                    spv::ExecutionModel::Vertex) {
                    return Status::SuccessWithoutChange;
                }

                auto* defUseMgr = irContext->get_def_use_mgr();
                auto* typeMgr = irContext->get_type_mgr();
                auto* constMgr = irContext->get_constant_mgr();

                struct Target {
                    Instruction* variable = nullptr;
                    Uint32 arrayTypeId = 0;
                    Uint32 elementTypeId = 0;
                    Uint32 elementCount = 0;
                    Uint32 baseLocation = 0;
                    Uint32 locationsPerElement = 1;
                    Uint32 elementInputPointerTypeId = 0;
                    Uint32 elementPrivatePointerTypeId = 0;
                    Uint32 arrayPrivatePointerTypeId = 0;
                };
                std::vector<Target> targets;

                for (Instruction& inst : irContext->types_values()) {
                    if (inst.opcode() != spv::Op::OpVariable) continue;
                    if (static_cast<spv::StorageClass>(inst.GetSingleWordInOperand(0)) !=
                        spv::StorageClass::Input) {
                        continue;
                    }
                    Instruction* pointerType = defUseMgr->GetDef(inst.type_id());
                    if (pointerType == nullptr) continue;
                    const Uint32 pointeeTypeId = pointerType->GetSingleWordInOperand(1);
                    const analysis::Type* pointeeType = typeMgr->GetType(pointeeTypeId);
                    const auto* arrayType = pointeeType != nullptr ? pointeeType->AsArray() : nullptr;
                    if (arrayType == nullptr) continue;

                    // A builtin input array (gl_ClipDistance and friends) is not an attribute and
                    // has no location; SPIRV-Cross emits those itself.
                    Uint32 baseLocation = 0;
                    if (!FindLocationDecoration(*irContext, inst.result_id(), baseLocation)) continue;

                    const Uint32 elementTypeId = typeMgr->GetId(arrayType->element_type());
                    const Uint32 locationsPerElement = LocationsPerElement(arrayType->element_type());
                    if (elementTypeId == 0 || locationsPerElement == 0) {
                        MGLOG_D("SplitArrayVertexInputsPass: vertex input %%%u is an array whose element "
                                "type has no single-location mapping; leaving it declared as an array",
                                inst.result_id());
                        continue;
                    }

                    // OpTypeArray's length is an id of an integer constant.
                    Instruction* arrayTypeInst = defUseMgr->GetDef(pointeeTypeId);
                    if (arrayTypeInst == nullptr || arrayTypeInst->NumInOperands() < 2) continue;
                    const analysis::Constant* lengthConstant =
                        constMgr->FindDeclaredConstant(arrayTypeInst->GetSingleWordInOperand(1));
                    if (lengthConstant == nullptr || lengthConstant->AsIntConstant() == nullptr) continue;
                    const Uint32 elementCount = lengthConstant->AsIntConstant()->GetU32();
                    if (elementCount == 0) continue;

                    Target target;
                    target.variable = &inst;
                    target.arrayTypeId = pointeeTypeId;
                    target.elementTypeId = elementTypeId;
                    target.elementCount = elementCount;
                    target.baseLocation = baseLocation;
                    target.locationsPerElement = locationsPerElement;
                    targets.push_back(target);
                }

                if (targets.empty()) return Status::SuccessWithoutChange;

                // Everything the demoted array's pointer flows into, in SSA order. Demoting the
                // variable changes the STORAGE CLASS of every pointer derived from it, and a
                // derived pointer's own result type still says Input - which is an invalid
                // module ("the result pointer storage class and base pointer storage class in
                // OpAccessChain do not match"), so each one has to be retyped too. Collected
                // before anything is mutated so an unsupported use can still decline the whole
                // rewrite rather than leave a half-converted module behind.
                std::unordered_set<Uint32> derivedPointers;
                for (const auto& target : targets) {
                    derivedPointers.insert(target.variable->result_id());
                }
                std::vector<Instruction*> pointersToRetype;
                for (auto& function : *irContext->module()) {
                    for (auto& block : function) {
                        for (auto& inst : block) {
                            const spv::Op opcode = inst.opcode();
                            // A pointer this pass moved may only be loaded from or indexed into.
                            // Anything else (handing it to a function, storing THROUGH it, casting
                            // it) either cannot happen for a vertex input or would need the callee
                            // rewritten as well, and silently getting that wrong is worse than
                            // leaving the module for SPIRV-Cross to reject out loud.
                            const Bool indexes =
                                opcode == spv::Op::OpAccessChain ||
                                opcode == spv::Op::OpInBoundsAccessChain ||
                                opcode == spv::Op::OpCopyObject;
                            if (indexes) {
                                if (inst.NumInOperands() > 0 &&
                                    derivedPointers.count(inst.GetSingleWordInOperand(0)) != 0) {
                                    derivedPointers.insert(inst.result_id());
                                    pointersToRetype.push_back(&inst);
                                }
                                continue;
                            }
                            if (opcode == spv::Op::OpLoad) continue; // reads are always fine
                            for (Uint32 i = 0; i < inst.NumInOperands(); ++i) {
                                const Operand& operand = inst.GetInOperand(i);
                                if (operand.type != SPV_OPERAND_TYPE_ID || operand.words.size() != 1) {
                                    continue;
                                }
                                if (derivedPointers.count(operand.words[0]) == 0) continue;
                                MGLOG_D("SplitArrayVertexInputsPass: array vertex input %%%u reaches a "
                                        "SPIR-V opcode %u that this pass cannot follow; leaving it "
                                        "declared as an "
                                        "array",
                                        operand.words[0], static_cast<Uint32>(opcode));
                                return Status::SuccessWithoutChange;
                            }
                        }
                    }
                }

                // Entry block insertion point: after the block's leading OpVariable run, which
                // SPIR-V requires to stay at the top of a function's first block.
                const Uint32 entryFunctionId = entryPoint->GetSingleWordInOperand(1);
                spvtools::opt::Function* entryFunction = nullptr;
                for (auto& function : *irContext->module()) {
                    if (function.result_id() == entryFunctionId) {
                        entryFunction = &function;
                        break;
                    }
                }
                if (entryFunction == nullptr || entryFunction->begin() == entryFunction->end()) {
                    return Status::SuccessWithoutChange;
                }
                auto& entryBlock = *entryFunction->begin();
                auto insertPoint = entryBlock.begin();
                while (insertPoint != entryBlock.end() && insertPoint->opcode() == spv::Op::OpVariable) {
                    ++insertPoint;
                }
                if (insertPoint == entryBlock.end()) return Status::SuccessWithoutChange;

                // Every type this pass names has to exist before the variables that name it: the
                // types-and-variables section is emitted in order and a forward reference to a
                // type is invalid SPIR-V. Same staging as PackDoubleVertexInputsPass.
                for (auto& target : targets) {
                    target.elementInputPointerTypeId =
                        typeMgr->FindPointerToType(target.elementTypeId, spv::StorageClass::Input);
                    target.elementPrivatePointerTypeId =
                        typeMgr->FindPointerToType(target.elementTypeId, spv::StorageClass::Private);
                    target.arrayPrivatePointerTypeId =
                        typeMgr->FindPointerToType(target.arrayTypeId, spv::StorageClass::Private);
                }

                // Index constants for the seeding access chains, all of them before any variable.
                Uint32 maxElementCount = 0;
                for (const auto& target : targets) {
                    maxElementCount = std::max(maxElementCount, target.elementCount);
                }
                std::vector<Uint32> indexConstantIds(maxElementCount, 0);
                for (Uint32 index = 0; index < maxElementCount; ++index) {
                    indexConstantIds[index] = constMgr->GetUIntConstId(index);
                }

                std::vector<Operand> interfaceOperands;
                for (Uint32 i = 0; i < entryPoint->NumInOperands(); ++i) {
                    interfaceOperands.push_back(entryPoint->GetInOperand(i));
                }

                for (const auto& target : targets) {
                    Instruction* variable = target.variable;
                    const Uint32 oldVariableId = variable->result_id();

                    // Collected, never added inline: AddAnnotationInst mutates the annotation
                    // list this function is still walking below.
                    std::vector<std::unique_ptr<Instruction>> newDecorations;
                    std::vector<Uint32> elementVariableIds(target.elementCount, 0);
                    for (Uint32 element = 0; element < target.elementCount; ++element) {
                        const Uint32 elementVariableId = irContext->TakeNextId();
                        elementVariableIds[element] = elementVariableId;
                        irContext->AddGlobalValue(spvtools::MakeUnique<Instruction>(
                            irContext, spv::Op::OpVariable, target.elementInputPointerTypeId,
                            elementVariableId,
                            std::initializer_list<Operand>{
                                {SPV_OPERAND_TYPE_STORAGE_CLASS,
                                 {static_cast<Uint32>(spv::StorageClass::Input)}}}));
                        newDecorations.push_back(spvtools::MakeUnique<Instruction>(
                            irContext, spv::Op::OpDecorate, 0, 0,
                            std::initializer_list<Operand>{
                                {SPV_OPERAND_TYPE_ID, {elementVariableId}},
                                {SPV_OPERAND_TYPE_DECORATION,
                                 {static_cast<Uint32>(spv::Decoration::Location)}},
                                {SPV_OPERAND_TYPE_LITERAL_INTEGER,
                                 {target.baseLocation + element * target.locationsPerElement}}}));
                    }

                    // The array's own decorations move to the elements, except Location (each
                    // element got its own above) and anything that only described the aggregate.
                    // RelaxedPrecision is the one that MUST travel: dropping it changes the
                    // declared precision of the input in the emitted ESSL.
                    std::vector<Instruction*> deadDecorations;
                    for (auto& annotation : irContext->annotations()) {
                        if (annotation.opcode() != spv::Op::OpDecorate) continue;
                        if (annotation.GetSingleWordInOperand(0) != oldVariableId) continue;
                        const auto decoration =
                            static_cast<spv::Decoration>(annotation.GetSingleWordInOperand(1));
                        if (decoration == spv::Decoration::RelaxedPrecision ||
                            decoration == spv::Decoration::Flat ||
                            decoration == spv::Decoration::NoPerspective ||
                            decoration == spv::Decoration::Centroid ||
                            decoration == spv::Decoration::Sample) {
                            for (const Uint32 elementVariableId : elementVariableIds) {
                                std::vector<Operand> operands;
                                operands.push_back({SPV_OPERAND_TYPE_ID, {elementVariableId}});
                                for (Uint32 i = 1; i < annotation.NumInOperands(); ++i) {
                                    operands.push_back(annotation.GetInOperand(i));
                                }
                                newDecorations.push_back(spvtools::MakeUnique<Instruction>(
                                    irContext, spv::Op::OpDecorate, 0, 0, operands));
                            }
                        }
                        deadDecorations.push_back(&annotation);
                    }
                    for (auto* annotation : deadDecorations) {
                        irContext->KillInst(annotation);
                    }
                    for (auto& decoration : newDecorations) {
                        irContext->AddAnnotationInst(std::move(decoration));
                    }

                    // Demote the original to a Private global: every existing OpLoad and
                    // OpAccessChain on it stays valid and keeps its array type, dynamic indices
                    // included. Moved to the end of the section for the same reason as in
                    // PackDoubleVertexInputsPass - a variable may not forward-reference its type.
                    variable->SetResultType(target.arrayPrivatePointerTypeId);
                    variable->SetInOperand(0, {static_cast<Uint32>(spv::StorageClass::Private)});
                    variable->RemoveFromList();
                    irContext->AddGlobalValue(std::unique_ptr<Instruction>(variable));

                    // SPIR-V 1.3 lists only Input/Output in the entry-point interface, and the
                    // array is no longer an Input: replace it with the elements in place, so the
                    // interface keeps one entry per live interface variable.
                    std::vector<Operand> rebuilt;
                    for (Uint32 i = 0; i < interfaceOperands.size(); ++i) {
                        const Operand& operand = interfaceOperands[i];
                        if (i >= 3 && operand.type == SPV_OPERAND_TYPE_ID &&
                            operand.words.size() == 1 && operand.words[0] == oldVariableId) {
                            for (const Uint32 elementVariableId : elementVariableIds) {
                                rebuilt.push_back({SPV_OPERAND_TYPE_ID, {elementVariableId}});
                            }
                            continue;
                        }
                        rebuilt.push_back(operand);
                    }
                    interfaceOperands = std::move(rebuilt);

                    // Seed the Private array once, at the top of the entry point, before any of
                    // the code that reads it.
                    for (Uint32 element = 0; element < target.elementCount; ++element) {
                        const Uint32 loadedId = irContext->TakeNextId();
                        const Uint32 elementPointerId = irContext->TakeNextId();
                        insertPoint = insertPoint.InsertBefore(spvtools::MakeUnique<Instruction>(
                            irContext, spv::Op::OpLoad, target.elementTypeId, loadedId,
                            std::initializer_list<Operand>{
                                {SPV_OPERAND_TYPE_ID, {elementVariableIds[element]}}}));
                        ++insertPoint;
                        insertPoint = insertPoint.InsertBefore(spvtools::MakeUnique<Instruction>(
                            irContext, spv::Op::OpAccessChain, target.elementPrivatePointerTypeId,
                            elementPointerId,
                            std::initializer_list<Operand>{
                                {SPV_OPERAND_TYPE_ID, {oldVariableId}},
                                {SPV_OPERAND_TYPE_ID, {indexConstantIds[element]}}}));
                        ++insertPoint;
                        insertPoint = insertPoint.InsertBefore(spvtools::MakeUnique<Instruction>(
                            irContext, spv::Op::OpStore, 0, 0,
                            std::initializer_list<Operand>{{SPV_OPERAND_TYPE_ID, {elementPointerId}},
                                                           {SPV_OPERAND_TYPE_ID, {loadedId}}}));
                        ++insertPoint;
                    }
                }

                entryPoint->SetInOperands(std::move(interfaceOperands));

                // Retype the derived pointers collected above. Done last, so the pointer types
                // it appends land after the variables (nothing in the types-and-variables
                // section names them - they are only ever the result type of an instruction in
                // a function body).
                for (Instruction* pointer : pointersToRetype) {
                    Instruction* resultType = defUseMgr->GetDef(pointer->type_id());
                    if (resultType == nullptr || resultType->opcode() != spv::Op::OpTypePointer) continue;
                    if (static_cast<spv::StorageClass>(resultType->GetSingleWordInOperand(0)) ==
                        spv::StorageClass::Private) {
                        continue;
                    }
                    pointer->SetResultType(typeMgr->FindPointerToType(resultType->GetSingleWordInOperand(1),
                                                                      spv::StorageClass::Private));
                }

                irContext->InvalidateAnalysesExceptFor(IRContext::kAnalysisNone);
                return Status::SuccessWithChange;
            }

            spvtools::Optimizer::PassToken SplitArrayVertexInputsPass::CreateSplitArrayVertexInputsPass() {
                return spvtools::Optimizer::PassToken(MakeUnique<SplitArrayVertexInputsPass>());
            }
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
