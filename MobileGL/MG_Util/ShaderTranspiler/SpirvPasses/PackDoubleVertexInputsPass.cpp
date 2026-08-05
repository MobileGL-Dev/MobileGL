// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/PackDoubleVertexInputsPass.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "PackDoubleVertexInputsPass.h"

#include "spirv.hpp"
#include "source/opt/def_use_manager.h"
#include "source/opt/instruction.h"
#include "source/opt/ir_context.h"
#include "source/opt/module.h"
#include "source/opt/types.h"
#include "source/util/make_unique.h"

#include <memory>
#include <vector>

namespace MobileGL {
    namespace MG_Util {
        namespace ShaderTranspiler {
            namespace {
                using spvtools::opt::IRContext;
                using spvtools::opt::Instruction;
                using spvtools::opt::Operand;
                namespace analysis = spvtools::opt::analysis;

                // Component count of a 64-bit float input, or 0 if the type is not one.
                Uint32 DoubleComponentCount(const analysis::Type* type) {
                    if (type == nullptr) return 0;
                    if (const auto* scalar = type->AsFloat()) {
                        return scalar->width() == 64 ? 1u : 0u;
                    }
                    if (const auto* vector = type->AsVector()) {
                        const auto* element = vector->element_type()->AsFloat();
                        if (element == nullptr || element->width() != 64) return 0;
                        return vector->element_count();
                    }
                    return 0;
                }
            } // namespace

            spvtools::opt::Pass::Status PackDoubleVertexInputsPass::Process() {
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

                struct Target {
                    Instruction* variable = nullptr;
                    Uint32 doubleTypeId = 0;
                    Uint32 componentCount = 0;
                    Uint32 packedTypeId = 0;
                    Uint32 packedPointerTypeId = 0;
                    Uint32 privatePointerTypeId = 0;
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
                    const Uint32 components = DoubleComponentCount(typeMgr->GetType(pointeeTypeId));
                    if (components == 0) continue;
                    if (components > 2) {
                        // 6 or 8 uint32 components has no single vertex format, and GL spreads such
                        // an input over two attribute locations. Left alone; the vertex-input
                        // factory declines the matching attribute for the same reason.
                        MGLOG_E("PackDoubleVertexInputsPass: vertex input %%%u is a %u-component 64-bit "
                                "float; only double and dvec2 inputs can be packed",
                                inst.result_id(), components);
                        continue;
                    }
                    targets.push_back({&inst, pointeeTypeId, components});
                }

                if (targets.empty()) return Status::SuccessWithoutChange;

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

                const Uint32 uintTypeId = typeMgr->GetUIntTypeId();
                const analysis::Integer* uintType = typeMgr->GetType(uintTypeId)->AsInteger();

                // Every type instruction has to exist before any variable that names it: the
                // types-and-variables section is walked in order and a forward reference to a type is
                // invalid SPIR-V. GetTypeInstruction/FindPointerToType append, and the variables are
                // appended (or re-appended) below, so all three lookups run first for every target.
                for (auto& target : targets) {
                    analysis::Vector packedVectorType(uintType, target.componentCount * 2u);
                    target.packedTypeId = typeMgr->GetTypeInstruction(&packedVectorType);
                    target.packedPointerTypeId =
                        typeMgr->FindPointerToType(target.packedTypeId, spv::StorageClass::Input);
                    target.privatePointerTypeId =
                        typeMgr->FindPointerToType(target.doubleTypeId, spv::StorageClass::Private);
                }

                for (const auto& target : targets) {
                    Instruction* variable = target.variable;
                    const Uint32 oldVariableId = variable->result_id();
                    const Uint32 packedTypeId = target.packedTypeId;
                    const Uint32 packedPointerTypeId = target.packedPointerTypeId;

                    const Uint32 packedVariableId = irContext->TakeNextId();
                    irContext->AddGlobalValue(spvtools::MakeUnique<Instruction>(
                        irContext, spv::Op::OpVariable, packedPointerTypeId, packedVariableId,
                        std::initializer_list<Operand>{
                            {SPV_OPERAND_TYPE_STORAGE_CLASS,
                             {static_cast<Uint32>(spv::StorageClass::Input)}}}));

                    // The interface decorations belong to whatever is actually the Input now.
                    std::vector<Instruction*> deadDecorations;
                    for (auto& annotation : irContext->annotations()) {
                        if (annotation.opcode() != spv::Op::OpDecorate) continue;
                        if (annotation.GetSingleWordInOperand(0) != oldVariableId) continue;
                        const auto decoration =
                            static_cast<spv::Decoration>(annotation.GetSingleWordInOperand(1));
                        if (decoration == spv::Decoration::Location ||
                            decoration == spv::Decoration::Component ||
                            decoration == spv::Decoration::RelaxedPrecision) {
                            annotation.SetInOperand(0, {packedVariableId});
                        } else {
                            deadDecorations.push_back(&annotation);
                        }
                    }
                    for (auto* annotation : deadDecorations) {
                        irContext->KillInst(annotation);
                    }

                    // Demote the original to a Private global: every existing OpLoad /
                    // OpAccessChain on it stays valid and keeps its double type. It is also moved to
                    // the end of the section, because the pointer-to-Private type it now names was
                    // appended above and a variable may not forward-reference its own type.
                    variable->SetResultType(target.privatePointerTypeId);
                    variable->SetInOperand(0, {static_cast<Uint32>(spv::StorageClass::Private)});
                    variable->RemoveFromList();
                    irContext->AddGlobalValue(std::unique_ptr<Instruction>(variable));

                    // SPIR-V 1.3 lists only Input/Output in the entry-point interface.
                    std::vector<Operand> interfaceOperands;
                    for (Uint32 i = 0; i < entryPoint->NumInOperands(); ++i) {
                        const Operand& operand = entryPoint->GetInOperand(i);
                        if (i >= 3 && operand.type == SPV_OPERAND_TYPE_ID &&
                            entryPoint->GetSingleWordInOperand(i) == oldVariableId) {
                            interfaceOperands.push_back({SPV_OPERAND_TYPE_ID, {packedVariableId}});
                            continue;
                        }
                        interfaceOperands.push_back(operand);
                    }
                    entryPoint->SetInOperands(std::move(interfaceOperands));

                    const Uint32 loadedId = irContext->TakeNextId();
                    const Uint32 bitcastId = irContext->TakeNextId();
                    insertPoint = insertPoint.InsertBefore(spvtools::MakeUnique<Instruction>(
                        irContext, spv::Op::OpLoad, packedTypeId, loadedId,
                        std::initializer_list<Operand>{{SPV_OPERAND_TYPE_ID, {packedVariableId}}}));
                    ++insertPoint;
                    insertPoint = insertPoint.InsertBefore(spvtools::MakeUnique<Instruction>(
                        irContext, spv::Op::OpBitcast, target.doubleTypeId, bitcastId,
                        std::initializer_list<Operand>{{SPV_OPERAND_TYPE_ID, {loadedId}}}));
                    ++insertPoint;
                    insertPoint = insertPoint.InsertBefore(spvtools::MakeUnique<Instruction>(
                        irContext, spv::Op::OpStore, 0, 0,
                        std::initializer_list<Operand>{{SPV_OPERAND_TYPE_ID, {oldVariableId}},
                                                       {SPV_OPERAND_TYPE_ID, {bitcastId}}}));
                    ++insertPoint;
                }

                irContext->InvalidateAnalysesExceptFor(IRContext::kAnalysisNone);
                return Status::SuccessWithChange;
            }

            spvtools::Optimizer::PassToken PackDoubleVertexInputsPass::CreatePackDoubleVertexInputsPass() {
                return spvtools::Optimizer::PassToken(MakeUnique<PackDoubleVertexInputsPass>());
            }
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
