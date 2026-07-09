// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/RebaseInstanceIndexPass.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "RebaseInstanceIndexPass.h"

#include "spirv.hpp"
#include "source/opt/def_use_manager.h"
#include "source/opt/instruction.h"
#include "source/opt/ir_context.h"
#include "source/opt/module.h"
#include "source/util/make_unique.h"

#include <vector>

namespace MobileGL {
    namespace MG_Util {
        namespace ShaderTranspiler {
            namespace {
                using spvtools::opt::Instruction;
                using spvtools::opt::IRContext;
                using spvtools::opt::Operand;

                // Returns the Input OpVariable decorated with |builtin|, or nullptr if none.
                Instruction* FindBuiltinInputVariable(IRContext* context, spv::BuiltIn builtin) {
                    auto* defUseMgr = context->get_def_use_mgr();
                    for (auto& annotation : context->annotations()) {
                        if (annotation.opcode() != spv::Op::OpDecorate || annotation.NumInOperands() < 3) {
                            continue;
                        }
                        if (static_cast<spv::Decoration>(annotation.GetSingleWordInOperand(1)) !=
                            spv::Decoration::BuiltIn) {
                            continue;
                        }
                        if (static_cast<spv::BuiltIn>(annotation.GetSingleWordInOperand(2)) != builtin) {
                            continue;
                        }

                        Instruction* variable = defUseMgr->GetDef(annotation.GetSingleWordInOperand(0));
                        if (variable == nullptr || variable->opcode() != spv::Op::OpVariable ||
                            static_cast<spv::StorageClass>(variable->GetSingleWordInOperand(0)) !=
                                spv::StorageClass::Input) {
                            continue;
                        }
                        return variable;
                    }
                    return nullptr;
                }

                // Creates an Input OpVariable decorated BuiltIn BaseInstance, reusing the
                // pointer-to-Input-int type of the existing InstanceIndex variable, and adds it
                // to every OpEntryPoint interface list. Returns the new variable's result id.
                uint32_t SynthesizeBaseInstanceVariable(IRContext* context, Instruction* instanceIndexVar) {
                    // gl_BaseInstance has the same type as gl_InstanceIndex (Input pointer to int);
                    // reuse the existing pointer type instead of creating a duplicate.
                    const uint32_t pointerTypeId = instanceIndexVar->type_id();
                    const uint32_t variableId = context->TakeNextId();

                    context->AddGlobalValue(spvtools::MakeUnique<Instruction>(
                        context, spv::Op::OpVariable, pointerTypeId, variableId,
                        std::initializer_list<Operand>{
                            {SPV_OPERAND_TYPE_STORAGE_CLASS,
                             {static_cast<uint32_t>(spv::StorageClass::Input)}}}));

                    context->AddAnnotationInst(spvtools::MakeUnique<Instruction>(
                        context, spv::Op::OpDecorate, 0, 0,
                        std::initializer_list<Operand>{
                            {SPV_OPERAND_TYPE_ID, {variableId}},
                            {SPV_OPERAND_TYPE_DECORATION,
                             {static_cast<uint32_t>(spv::Decoration::BuiltIn)}},
                            {SPV_OPERAND_TYPE_LITERAL_INTEGER,
                             {static_cast<uint32_t>(spv::BuiltIn::BaseInstance)}}}));

                    // Input variables must appear in the entry point interface list.
                    for (Instruction& entryPoint : context->module()->entry_points()) {
                        entryPoint.AddOperand({SPV_OPERAND_TYPE_ID, {variableId}});
                    }

                    return variableId;
                }
            } // namespace

            spvtools::opt::Pass::Status RebaseInstanceIndexPass::Process() {
                auto* irContext = context();
                auto* defUseMgr = irContext->get_def_use_mgr();

                Instruction* instanceIndexVar = FindBuiltinInputVariable(irContext, spv::BuiltIn::InstanceIndex);
                if (instanceIndexVar == nullptr) {
                    return Status::SuccessWithoutChange;
                }
                const uint32_t instanceIndexVarId = instanceIndexVar->result_id();

                // Collect every load of the InstanceIndex variable before mutating anything.
                std::vector<Instruction*> instanceLoads;
                defUseMgr->ForEachUser(instanceIndexVar, [&](Instruction* user) {
                    if (user->opcode() == spv::Op::OpLoad &&
                        user->GetSingleWordInOperand(0) == instanceIndexVarId) {
                        instanceLoads.push_back(user);
                    }
                });

                if (instanceLoads.empty()) {
                    // The builtin is declared but never read; nothing to rebase.
                    return Status::SuccessWithoutChange;
                }

                // Referencing BaseInstance requires the DrawParameters capability. On the SPIR-V
                // 1.3 target used here it is core, so no OpExtension is needed.
                irContext->AddCapability(spv::Capability::DrawParameters);

                Instruction* baseInstanceVar = FindBuiltinInputVariable(irContext, spv::BuiltIn::BaseInstance);
                const uint32_t baseInstanceVarId = (baseInstanceVar != nullptr)
                    ? baseInstanceVar->result_id()
                    : SynthesizeBaseInstanceVariable(irContext, instanceIndexVar);

                // Replace each `OpLoad %ty %res %instanceIndex` with
                //   OpLoad %ty %fresh1 %instanceIndex
                //   OpLoad %ty %fresh2 %baseInstance
                //   OpISub %ty %res   %fresh1 %fresh2
                // Reusing %res on the OpISub keeps downstream use lists intact.
                for (Instruction* loadInst : instanceLoads) {
                    const uint32_t typeId = loadInst->type_id();
                    const uint32_t instanceValueId = irContext->TakeNextId();
                    const uint32_t baseValueId = irContext->TakeNextId();

                    loadInst->InsertBefore(spvtools::MakeUnique<Instruction>(
                        irContext, spv::Op::OpLoad, typeId, instanceValueId,
                        std::initializer_list<Operand>{{SPV_OPERAND_TYPE_ID, {instanceIndexVarId}}}));
                    loadInst->InsertBefore(spvtools::MakeUnique<Instruction>(
                        irContext, spv::Op::OpLoad, typeId, baseValueId,
                        std::initializer_list<Operand>{{SPV_OPERAND_TYPE_ID, {baseInstanceVarId}}}));

                    loadInst->SetOpcode(spv::Op::OpISub);
                    loadInst->SetInOperands(Instruction::OperandList{
                        {SPV_OPERAND_TYPE_ID, {instanceValueId}},
                        {SPV_OPERAND_TYPE_ID, {baseValueId}}});
                }

                irContext->InvalidateAnalysesExceptFor(IRContext::kAnalysisNone);
                return Status::SuccessWithChange;
            }

            spvtools::Optimizer::PassToken RebaseInstanceIndexPass::CreateRebaseInstanceIndexPass() {
                return spvtools::Optimizer::PassToken(MakeUnique<RebaseInstanceIndexPass>());
            }
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
