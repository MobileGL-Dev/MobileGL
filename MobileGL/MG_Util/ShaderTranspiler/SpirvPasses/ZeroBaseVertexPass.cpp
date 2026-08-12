// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/ZeroBaseVertexPass.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "ZeroBaseVertexPass.h"

#include "spirv.hpp"
#include "source/opt/constants.h"
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
            } // namespace

            spvtools::opt::Pass::Status ZeroBaseVertexPass::Process() {
                auto* irContext = context();
                auto* defUseMgr = irContext->get_def_use_mgr();

                Instruction* baseVertexVar = FindBuiltinInputVariable(irContext, spv::BuiltIn::BaseVertex);
                if (baseVertexVar == nullptr) {
                    return Status::SuccessWithoutChange;
                }
                const uint32_t baseVertexVarId = baseVertexVar->result_id();

                // Collect every load before mutating: rewriting invalidates the use list.
                //
                // Every OTHER kind of user is enumerated and refused rather than ignored. A read
                // that reaches the variable through a copied pointer or a pointer function
                // parameter would keep Vulkan's firstVertex while the pass still reported
                // success, i.e. a partial rewrite indistinguishable from a complete one. glslang
                // emits neither shape from GLSL today, so this fails closed on something that
                // cannot happen yet rather than silently half-doing it when it can.
                std::vector<Instruction*> baseVertexLoads;
                Bool sawUnexpectedUser = false;
                defUseMgr->ForEachUser(baseVertexVar, [&](Instruction* user) {
                    switch (user->opcode()) {
                    case spv::Op::OpLoad:
                        if (user->GetSingleWordInOperand(0) == baseVertexVarId) {
                            baseVertexLoads.push_back(user);
                        } else {
                            sawUnexpectedUser = true;
                        }
                        return;
                    // Declarations of the variable, not reads of it.
                    case spv::Op::OpDecorate:
                    case spv::Op::OpDecorateId:
                    case spv::Op::OpDecorateString:
                    case spv::Op::OpName:
                    case spv::Op::OpEntryPoint:
                        return;
                    default:
                        sawUnexpectedUser = true;
                        return;
                    }
                });

                if (sawUnexpectedUser) {
                    return Status::Failure;
                }
                if (baseVertexLoads.empty()) {
                    // Declared but never read - the variant is already the shader itself.
                    return Status::SuccessWithoutChange;
                }

                // Materialize every zero constant BEFORE touching a single instruction, so the
                // constant/type managers are never consulted against a module this pass has
                // already half-rewritten - and so the rewrite loop below cannot fail partway
                // and leave one behind.
                auto* constantMgr = irContext->get_constant_mgr();
                auto* typeMgr = irContext->get_type_mgr();
                std::vector<uint32_t> zeroIds(baseVertexLoads.size(), 0);
                for (size_t i = 0; i < baseVertexLoads.size(); ++i) {
                    // The zero is built from the LOAD's own type, because a shader may declare
                    // the builtin as either int or uint.
                    const uint32_t typeId = baseVertexLoads[i]->type_id();
                    const spvtools::opt::analysis::Type* type = typeMgr->GetType(typeId);
                    if (type == nullptr) {
                        return Status::Failure;
                    }
                    const spvtools::opt::analysis::Constant* zero = constantMgr->GetConstant(type, {0u});
                    if (zero == nullptr) {
                        return Status::Failure;
                    }
                    const Instruction* zeroInst = constantMgr->GetDefiningInstruction(zero, typeId);
                    if (zeroInst == nullptr) {
                        return Status::Failure;
                    }
                    zeroIds[i] = zeroInst->result_id();
                }

                // `OpLoad %ty %res %baseVertex` becomes `OpCopyObject %ty %res %zero`. Keeping
                // %res makes every downstream use pick the zero up with no further rewriting.
                for (size_t i = 0; i < baseVertexLoads.size(); ++i) {
                    baseVertexLoads[i]->SetOpcode(spv::Op::OpCopyObject);
                    baseVertexLoads[i]->SetInOperands(Instruction::OperandList{
                        {SPV_OPERAND_TYPE_ID, {zeroIds[i]}}});
                }

                irContext->InvalidateAnalysesExceptFor(IRContext::kAnalysisNone);
                return Status::SuccessWithChange;
            }

            spvtools::Optimizer::PassToken ZeroBaseVertexPass::CreateZeroBaseVertexPass() {
                return spvtools::Optimizer::PassToken(MakeUnique<ZeroBaseVertexPass>());
            }
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
