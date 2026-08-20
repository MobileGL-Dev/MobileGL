// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/LowerViewportIndexPass.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "LowerViewportIndexPass.h"

#include "spirv.hpp"
#include "source/opt/build_module.h"
#include "source/opt/def_use_manager.h"
#include "source/opt/instruction.h"
#include "source/opt/ir_context.h"
#include "source/opt/module.h"
#include "source/util/make_unique.h"

#include <memory>
#include <vector>

namespace MobileGL {
    namespace MG_Util {
        namespace ShaderTranspiler {
            namespace {
                using spvtools::opt::Instruction;
                using spvtools::opt::IRContext;
                using spvtools::opt::Operand;

                // The name the decompiled ESSL ends up declaring. Same mg_ prefix as the
                // draw-parameter lowering, so a global that came from a demoted builtin is
                // recognisable in a driver log.
                constexpr const char* kLoweredName = "mg_ViewportIndex";

                // The one decoration this pass lowers. OpDecorate only, never OpMemberDecorate:
                // glslang emits gl_ViewportIndex as a standalone variable, and a member of a
                // gl_PerVertex-shaped block could not be demoted on its own anyway. BuiltIn Layer
                // is deliberately not matched - see the header.
                Bool IsViewportIndexBuiltinDecoration(const Instruction& annotation) {
                    if (annotation.opcode() != spv::Op::OpDecorate ||
                        static_cast<spv::Decoration>(annotation.GetSingleWordInOperand(1)) !=
                            spv::Decoration::BuiltIn) {
                        return false;
                    }
                    return static_cast<spv::BuiltIn>(annotation.GetSingleWordInOperand(2)) ==
                           spv::BuiltIn::ViewportIndex;
                }

                // The OUTPUT variable that decoration names, or nullptr. Only an output is
                // demotable: a fragment stage READS gl_ViewportIndex as an Input, and a Private
                // global has no defined value to read, so lowering that one would answer the
                // shader with garbage instead of the viewport it asked for. That case is left for
                // the driver to reject.
                Instruction* GetDecoratedViewportIndexOutput(IRContext* context,
                                                             const Instruction& annotation) {
                    Instruction* variable =
                        context->get_def_use_mgr()->GetDef(annotation.GetSingleWordInOperand(0));
                    if (variable == nullptr || variable->opcode() != spv::Op::OpVariable ||
                        static_cast<spv::StorageClass>(variable->GetSingleWordInOperand(0)) !=
                            spv::StorageClass::Output) {
                        return nullptr;
                    }
                    return variable;
                }

                // Decorations the validator accepts only on an Input/Output variable, so they have
                // to go with the storage class or the demoted module stops validating. glslang
                // puts none of these on gl_ViewportIndex today - the BuiltIn is all it writes -
                // but a geometry `layout(stream = N)` qualifier decorates every output of the
                // stage, and the pass must not be the thing that produces an invalid module.
                Bool IsInterfaceOnlyDecoration(spv::Decoration decoration) {
                    switch (decoration) {
                    case spv::Decoration::Flat:
                    case spv::Decoration::NoPerspective:
                    case spv::Decoration::Centroid:
                    case spv::Decoration::Sample:
                    case spv::Decoration::Patch:
                    case spv::Decoration::Invariant:
                    case spv::Decoration::Location:
                    case spv::Decoration::Component:
                    case spv::Decoration::Stream:
                    case spv::Decoration::XfbBuffer:
                    case spv::Decoration::XfbStride:
                        return true;
                    default:
                        return false;
                    }
                }

                void ReplaceName(IRContext* context, uint32_t id, const char* name) {
                    for (auto& debugInst : context->debugs2()) {
                        if (debugInst.opcode() == spv::Op::OpName && debugInst.GetSingleWordInOperand(0) == id) {
                            debugInst.SetInOperand(
                                1, spvtools::utils::MakeVector<spvtools::opt::Operand::OperandData>(name));
                            return;
                        }
                    }
                    context->AddDebug2Inst(spvtools::MakeUnique<Instruction>(
                        context, spv::Op::OpName, 0, 0,
                        std::initializer_list<Operand>{
                            {SPV_OPERAND_TYPE_ID, {id}},
                            {SPV_OPERAND_TYPE_LITERAL_STRING, spvtools::utils::MakeVector(name)}}));
                }

                void RemoveFromEntryPointInterfaces(IRContext* context, uint32_t id) {
                    for (Instruction& entryPoint : context->module()->entry_points()) {
                        std::vector<Operand> newOperands;
                        Bool changed = false;
                        for (uint32_t i = 0; i < entryPoint.NumInOperands(); ++i) {
                            const Operand& operand = entryPoint.GetInOperand(i);
                            // Interface ids start after execution model, entry-point id and name.
                            if (i >= 3 && operand.type == SPV_OPERAND_TYPE_ID &&
                                entryPoint.GetSingleWordInOperand(i) == id) {
                                changed = true;
                                continue;
                            }
                            newOperands.push_back(operand);
                        }
                        if (changed) {
                            entryPoint.SetInOperands(std::move(newOperands));
                        }
                    }
                }
            } // namespace

            bool LowerViewportIndexPass::DeclaresViewportIndexBuiltin(const Vector<Uint32>& binary) {
                if (binary.empty()) {
                    // An empty module is a stage that produced no SPIR-V, which is not a verdict
                    // about viewport routing; letting BuildModule reject it would push a spurious
                    // diagnostic through the message consumer first.
                    return false;
                }
                std::unique_ptr<IRContext> context = spvtools::BuildModule(
                    SPV_ENV_VULKAN_1_1, [](spv_message_level_t, const char*, const spv_position_t&, const char*) {},
                    binary.data(), binary.size());
                if (!context) {
                    // Unparseable here means unusable downstream too; let the ordinary transpile
                    // path produce the error rather than inventing a verdict from it.
                    return false;
                }
                for (const Instruction& annotation : context->annotations()) {
                    if (IsViewportIndexBuiltinDecoration(annotation) &&
                        GetDecoratedViewportIndexOutput(context.get(), annotation) != nullptr) {
                        return true;
                    }
                }
                return false;
            }

            spvtools::opt::Pass::Status LowerViewportIndexPass::Process() {
                auto* irContext = context();

                // Collect the decorations to lower first; mutating while iterating annotations
                // invalidates the range.
                struct LoweredVariable {
                    Instruction* variable = nullptr;
                    Instruction* decoration = nullptr;
                };
                std::vector<LoweredVariable> targets;

                for (auto& annotation : irContext->annotations()) {
                    if (!IsViewportIndexBuiltinDecoration(annotation)) {
                        continue;
                    }

                    Instruction* variable = GetDecoratedViewportIndexOutput(irContext, annotation);
                    if (variable == nullptr) {
                        continue;
                    }

                    targets.push_back({variable, &annotation});
                }

                if (targets.empty()) {
                    return Status::SuccessWithoutChange;
                }

                // Second collection pass, for the same reason as the first: the decorations that
                // stop being legal once the variable leaves the Output storage class.
                std::vector<Instruction*> deadDecorations;
                for (auto& annotation : irContext->annotations()) {
                    if (annotation.opcode() != spv::Op::OpDecorate ||
                        !IsInterfaceOnlyDecoration(
                            static_cast<spv::Decoration>(annotation.GetSingleWordInOperand(1)))) {
                        continue;
                    }
                    const uint32_t decoratedId = annotation.GetSingleWordInOperand(0);
                    for (const auto& target : targets) {
                        if (target.variable->result_id() == decoratedId) {
                            deadDecorations.push_back(&annotation);
                            break;
                        }
                    }
                }

                auto* defUseMgr = irContext->get_def_use_mgr();
                auto* typeMgr = irContext->get_type_mgr();

                for (auto& target : targets) {
                    Instruction* variable = target.variable;
                    const uint32_t variableId = variable->result_id();

                    // Demote the Output builtin to a plain Private global. Every store the shader
                    // already makes stays exactly where it is - it simply no longer reaches the
                    // rasterizer, which is the whole of the degradation.
                    Instruction* pointerType = defUseMgr->GetDef(variable->type_id());
                    const uint32_t pointeeTypeId = pointerType->GetSingleWordInOperand(1);
                    const uint32_t privatePointerTypeId =
                        typeMgr->FindPointerToType(pointeeTypeId, spv::StorageClass::Private);
                    variable->SetResultType(privatePointerTypeId);
                    variable->SetInOperand(0, {static_cast<uint32_t>(spv::StorageClass::Private)});

                    // FindPointerToType APPENDS a newly minted pointer type to the end of the
                    // globals section - after this variable - and SPIR-V requires def before use.
                    // Re-anchor the variable directly after its new type, which is equally correct
                    // when the type already existed further up.
                    Instruction* privatePointerType = defUseMgr->GetDef(privatePointerTypeId);
                    variable->RemoveFromList();
                    variable->InsertAfter(privatePointerType);

                    irContext->KillInst(target.decoration);
                    RemoveFromEntryPointInterfaces(irContext, variableId);
                    ReplaceName(irContext, variableId, kLoweredName);
                }

                for (auto* decoration : deadDecorations) {
                    irContext->KillInst(decoration);
                }

                // The MultiViewport / ShaderViewportIndexLayerEXT capabilities are deliberately
                // left declared, unlike DrawParameters in the sibling pass. They are not exclusive
                // to this builtin: ShaderViewportIndexLayerEXT also enables gl_Layer in the
                // pre-geometry stages, and it DEPENDS on MultiViewport, so dropping either can
                // invalidate a module that still writes Layer. A declared-but-unused capability is
                // legal SPIR-V and SPIRV-Cross's GLSL backend reads neither of them, so leaving
                // both costs nothing.

                return Status::SuccessWithChange;
            }

            spvtools::Optimizer::PassToken LowerViewportIndexPass::CreateLowerViewportIndexPass() {
                return spvtools::Optimizer::PassToken(MakeUnique<LowerViewportIndexPass>());
            }
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
