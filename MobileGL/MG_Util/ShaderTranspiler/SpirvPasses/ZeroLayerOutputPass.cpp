// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/ZeroLayerOutputPass.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "ZeroLayerOutputPass.h"

#include "spirv.hpp"
#include "source/opt/constants.h"
#include "source/opt/def_use_manager.h"
#include "source/opt/instruction.h"
#include "source/opt/ir_context.h"
#include "source/opt/module.h"
#include "source/opt/type_manager.h"
#include "source/util/make_unique.h"

#include <vector>

namespace MobileGL {
    namespace MG_Util {
        namespace ShaderTranspiler {
            namespace {
                using spvtools::opt::Instruction;
                using spvtools::opt::IRContext;

                // Returns the Output OpVariable decorated BuiltIn Layer, or nullptr if none.
                Instruction* FindLayerOutputVariable(IRContext* context) {
                    auto* defUseMgr = context->get_def_use_mgr();
                    for (auto& annotation : context->annotations()) {
                        if (annotation.opcode() != spv::Op::OpDecorate || annotation.NumInOperands() < 3) continue;
                        if (static_cast<spv::Decoration>(annotation.GetSingleWordInOperand(1)) !=
                            spv::Decoration::BuiltIn)
                            continue;
                        if (static_cast<spv::BuiltIn>(annotation.GetSingleWordInOperand(2)) != spv::BuiltIn::Layer)
                            continue;
                        Instruction* variable = defUseMgr->GetDef(annotation.GetSingleWordInOperand(0));
                        if (variable != nullptr && variable->opcode() == spv::Op::OpVariable &&
                            static_cast<spv::StorageClass>(variable->GetSingleWordInOperand(0)) ==
                                spv::StorageClass::Output) {
                            return variable;
                        }
                    }
                    return nullptr;
                }

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

            spvtools::opt::Pass::Status ZeroLayerOutputPass::Process() {
                auto* irContext = context();
                auto* defUseMgr = irContext->get_def_use_mgr();
                auto* constantMgr = irContext->get_constant_mgr();
                auto* typeMgr = irContext->get_type_mgr();

                Instruction* layer = FindLayerOutputVariable(irContext);
                if (layer == nullptr) return Status::SuccessWithoutChange;

                // Gather the stores before mutating anything, so a shape the pass refuses leaves
                // the module exactly as it was.
                const uint32_t layerId = layer->result_id();
                std::vector<Instruction*> stores;
                Bool refused = false;
                defUseMgr->ForEachUser(layer, [&](Instruction* user) {
                    if (refused || IsDeclarationUse(user)) return;
                    if (user->opcode() == spv::Op::OpStore && user->GetSingleWordInOperand(0) == layerId) {
                        stores.push_back(user);
                        return;
                    }
                    refused = true;
                });
                if (refused) return Status::Failure;
                if (stores.empty()) return Status::SuccessWithoutChange;

                const auto* pointerType = typeMgr->GetType(layer->type_id());
                const auto* pointer = pointerType ? pointerType->AsPointer() : nullptr;
                const auto* valueType = pointer ? pointer->pointee_type() : nullptr;
                if (valueType == nullptr || valueType->AsInteger() == nullptr) return Status::Failure;
                const uint32_t valueTypeId = typeMgr->GetId(valueType);
                const auto* zero = constantMgr->GetConstant(valueType, {0u});
                const Instruction* zeroInst = zero ? constantMgr->GetDefiningInstruction(zero, valueTypeId) : nullptr;
                if (valueTypeId == 0 || zeroInst == nullptr) return Status::Failure;
                const uint32_t zeroId = zeroInst->result_id();

                for (Instruction* store : stores)
                    store->SetInOperand(1, {zeroId});

                irContext->InvalidateAnalysesExceptFor(IRContext::kAnalysisNone);
                return Status::SuccessWithChange;
            }

            spvtools::Optimizer::PassToken ZeroLayerOutputPass::CreateZeroLayerOutputPass() {
                return spvtools::Optimizer::PassToken(MakeUnique<ZeroLayerOutputPass>());
            }
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
