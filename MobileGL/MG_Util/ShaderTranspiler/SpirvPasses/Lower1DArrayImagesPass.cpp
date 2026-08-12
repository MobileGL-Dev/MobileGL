// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/Lower1DArrayImagesPass.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "Lower1DArrayImagesPass.h"

#include "spirv.hpp"
#include "source/opt/build_module.h"
#include "source/opt/constants.h"
#include "source/opt/def_use_manager.h"
#include "source/opt/instruction.h"
#include "source/opt/ir_builder.h"
#include "source/opt/ir_context.h"
#include "source/opt/module.h"
#include "source/opt/type_manager.h"
#include "source/opt/types.h"
#include "source/util/make_unique.h"

#include <memory>
#include <vector>

namespace MobileGL {
    namespace MG_Util {
        namespace ShaderTranspiler {
            namespace {
                using spvtools::opt::Instruction;
                using spvtools::opt::InstructionBuilder;
                using spvtools::opt::IRContext;

                // OpTypeImage in-operands: 0 sampled type, 1 Dim, 2 Depth, 3 Arrayed, 4 MS,
                // 5 Sampled, 6 Format.
                constexpr uint32_t kDimOperand = 1;
                constexpr uint32_t kArrayedOperand = 3;
                constexpr uint32_t kSampledOperand = 5;

                // A 1D image that is arrayed AND is a storage image. Sampled == 2 is SPIR-V's
                // "used without a sampler", i.e. exactly the image uniforms this pass exists for;
                // Sampled == 1 (a sampled image) reaches SPIRV-Cross's sampler path, which
                // already handles the 1D-array shape correctly and must be left to it.
                bool Is1DArrayStorageImageType(const Instruction* imageType) {
                    return imageType != nullptr && imageType->opcode() == spv::Op::OpTypeImage &&
                           imageType->NumInOperands() > kSampledOperand &&
                           static_cast<spv::Dim>(imageType->GetSingleWordInOperand(kDimOperand)) == spv::Dim::Dim1D &&
                           imageType->GetSingleWordInOperand(kArrayedOperand) == 1u &&
                           imageType->GetSingleWordInOperand(kSampledOperand) == 2u;
                }

                // Any Dim1D image, sampled or storage. Used only to decide whether the Image1D
                // capability is still needed - deliberately wider than the rewrite's own
                // predicate, so a module that also holds a non-arrayed 1D image (which this pass
                // leaves to SPIRV-Cross) keeps the capability it still requires.
                bool IsDim1DImageType(const Instruction* imageType) {
                    return imageType != nullptr && imageType->opcode() == spv::Op::OpTypeImage &&
                           imageType->NumInOperands() > kSampledOperand &&
                           static_cast<spv::Dim>(imageType->GetSingleWordInOperand(kDimOperand)) == spv::Dim::Dim1D;
                }

                // The OpTypeImage behind whatever an image operation was handed - a bare image,
                // or a pointer to one. Same unwrapping as NormalizeRectCoordinatesPass, minus the
                // sampled-image case a storage image never has.
                Instruction* ResolveImageType(IRContext* context, uint32_t objectId) {
                    auto* defUseMgr = context->get_def_use_mgr();
                    Instruction* object = defUseMgr->GetDef(objectId);
                    if (object == nullptr) return nullptr;
                    Instruction* type = defUseMgr->GetDef(object->type_id());
                    while (type != nullptr) {
                        switch (type->opcode()) {
                        case spv::Op::OpTypeImage:
                            return type;
                        case spv::Op::OpTypeSampledImage:
                        case spv::Op::OpTypePointer:
                        case spv::Op::OpTypeArray:
                        case spv::Op::OpTypeRuntimeArray:
                            // Each names its element type in its last in-operand, except arrays,
                            // whose element type is the FIRST. Both are reached here because an
                            // image uniform may be declared as an array of images.
                            type = defUseMgr->GetDef(type->opcode() == spv::Op::OpTypeArray ||
                                                             type->opcode() == spv::Op::OpTypeRuntimeArray
                                                         ? type->GetSingleWordInOperand(0)
                                                         : type->GetSingleWordInOperand(type->NumInOperands() - 1));
                            continue;
                        default:
                            return nullptr;
                        }
                    }
                    return nullptr;
                }

                // The coordinate operand index for the operations that address an image's texels.
                // OpImageRead and OpImageTexelPointer take (image, coordinate, ...); OpImageWrite
                // takes (image, coordinate, texel).
                bool TryGetCoordinateOperand(spv::Op opcode, uint32_t* coordinateOperand) {
                    switch (opcode) {
                    case spv::Op::OpImageRead:
                    case spv::Op::OpImageSparseRead:
                    case spv::Op::OpImageWrite:
                    case spv::Op::OpImageTexelPointer:
                        *coordinateOperand = 1;
                        return true;
                    default:
                        return false;
                    }
                }

                bool QueriesImageSize(spv::Op opcode) {
                    return opcode == spv::Op::OpImageQuerySize || opcode == spv::Op::OpImageQuerySizeLod ||
                           opcode == spv::Op::OpImageQueryLevels || opcode == spv::Op::OpImageQuerySamples;
                }
            } // namespace

            Lower1DArrayImagesPass::ModuleTraits Lower1DArrayImagesPass::InspectBinary(const Vector<Uint32>& binary) {
                ModuleTraits traits{};
                if (binary.empty()) {
                    return traits;
                }
                std::unique_ptr<IRContext> context = spvtools::BuildModule(
                    SPV_ENV_VULKAN_1_1, [](spv_message_level_t, const char*, const spv_position_t&, const char*) {},
                    binary.data(), binary.size());
                if (!context) {
                    return traits;
                }

                // The type table settles it for the cheap half, and it is the half almost every
                // shader takes: no such type declared, nothing to inspect further.
                for (const Instruction& type : context->module()->types_values()) {
                    if (Is1DArrayStorageImageType(&type)) {
                        traits.declaresImage = true;
                        break;
                    }
                }
                if (!traits.declaresImage) {
                    return traits;
                }

                for (auto& function : *context->module()) {
                    for (auto& block : function) {
                        for (auto& instruction : block) {
                            if (!QueriesImageSize(instruction.opcode()) || instruction.NumInOperands() < 1) {
                                continue;
                            }
                            if (Is1DArrayStorageImageType(
                                    ResolveImageType(context.get(), instruction.GetSingleWordInOperand(0)))) {
                                traits.queriesImageSize = true;
                                return traits;
                            }
                        }
                    }
                }
                return traits;
            }

            spvtools::opt::Pass::Status Lower1DArrayImagesPass::Process() {
                auto* irContext = context();
                auto* typeMgr = irContext->get_type_mgr();
                auto* constantMgr = irContext->get_constant_mgr();

                // Nothing to do unless the module actually declares one. Every other shader pays
                // one walk of the type table and is handed back unchanged.
                bool hasType = false;
                for (const Instruction& type : irContext->types_values()) {
                    if (Is1DArrayStorageImageType(&type)) {
                        hasType = true;
                        break;
                    }
                }
                if (!hasType) {
                    return Status::SuccessWithoutChange;
                }

                // The same refusal the caller makes, restated here so the pass is safe wherever
                // it is registered. Rewriting the type while leaving an OpImageQuerySize on it
                // produces a query whose result type has one component too few - an invalid
                // module - and there is no correct two-component size to substitute, because the
                // ES texture genuinely has a height the GL one does not.
                for (auto& function : *irContext->module()) {
                    for (auto& block : function) {
                        for (auto& instruction : block) {
                            if (QueriesImageSize(instruction.opcode()) && instruction.NumInOperands() >= 1 &&
                                Is1DArrayStorageImageType(
                                    ResolveImageType(irContext, instruction.GetSingleWordInOperand(0)))) {
                                return Status::SuccessWithoutChange;
                            }
                        }
                    }
                }

                // (u, layer) -> (u, 0, layer). The height the ES 2D array carries is 1, so Y is
                // always 0 and the layer has to move from the second component to the third; a
                // plain widening that appended the 0 would read layer 0 of every access instead.
                for (auto& function : *irContext->module()) {
                    for (auto& block : function) {
                        for (auto& instruction : block) {
                            uint32_t coordinateOperand = 0;
                            if (!TryGetCoordinateOperand(instruction.opcode(), &coordinateOperand) ||
                                instruction.NumInOperands() <= coordinateOperand) {
                                continue;
                            }
                            if (!Is1DArrayStorageImageType(
                                    ResolveImageType(irContext, instruction.GetSingleWordInOperand(0)))) {
                                continue;
                            }

                            const uint32_t coordinateId = instruction.GetSingleWordInOperand(coordinateOperand);

                            // Built from the COORDINATE's own component type rather than a
                            // hardcoded signed int. GLSL only ever spells these ivec2, but SPIR-V
                            // permits an unsigned coordinate, and extracting a uint component
                            // into an int result is an invalid module rather than a wrong answer -
                            // the kind of defect that reaches a driver as "compiles here, not
                            // there".
                            Instruction* coordinateDef = irContext->get_def_use_mgr()->GetDef(coordinateId);
                            if (coordinateDef == nullptr) return Status::Failure;
                            const auto* coordinateType = typeMgr->GetType(coordinateDef->type_id());
                            const auto* coordinateVector = coordinateType != nullptr ? coordinateType->AsVector()
                                                                                     : nullptr;
                            if (coordinateVector == nullptr || coordinateVector->element_count() != 2) {
                                return Status::Failure;
                            }
                            const auto* component = coordinateVector->element_type();
                            const auto* componentInteger = component != nullptr ? component->AsInteger() : nullptr;
                            if (componentInteger == nullptr) return Status::Failure;

                            spvtools::opt::analysis::Vector widenedVector(component, 3);
                            const uint32_t int3TypeId = typeMgr->GetTypeInstruction(&widenedVector);
                            const uint32_t intTypeId = typeMgr->GetTypeInstruction(component);
                            const uint32_t zeroId = componentInteger->IsSigned()
                                                        ? constantMgr->GetSIntConstId(0)
                                                        : constantMgr->GetUIntConstId(0);
                            if (int3TypeId == 0 || intTypeId == 0 || zeroId == 0) {
                                return Status::Failure;
                            }

                            InstructionBuilder builder(
                                irContext, &instruction,
                                IRContext::kAnalysisDefUse | IRContext::kAnalysisInstrToBlockMapping);

                            Instruction* u =
                                builder.AddCompositeExtract(intTypeId, coordinateId, {0});
                            Instruction* layer =
                                builder.AddCompositeExtract(intTypeId, coordinateId, {1});
                            if (u == nullptr || layer == nullptr) {
                                return Status::Failure;
                            }
                            Instruction* widened = builder.AddCompositeConstruct(
                                int3TypeId, {u->result_id(), zeroId, layer->result_id()});
                            if (widened == nullptr) {
                                return Status::Failure;
                            }
                            instruction.SetInOperand(coordinateOperand, {widened->result_id()});
                            irContext->UpdateDefUse(&instruction);
                        }
                    }
                }

                // Only now, with no access still spelling the 1D-array coordinate, does the type
                // become the 2D array one. Arrayed stays 1: this is a 2D ARRAY image, which is
                // what the texture was stored as.
                for (Instruction& type : irContext->types_values()) {
                    if (Is1DArrayStorageImageType(&type)) {
                        type.SetInOperand(kDimOperand, {static_cast<uint32_t>(spv::Dim::Dim2D)});
                    }
                }

                // Image1D describes the types just rewritten - but only drop it if no 1D image
                // type is left at all. A module may hold a non-arrayed 1D storage image, which
                // this pass deliberately leaves to SPIRV-Cross, and that one still needs the
                // capability. Shader is always declared by any module reaching here, so restating
                // it keeps the instruction valid without leaving a capability a consumer could
                // key off.
                bool anyDim1DLeft = false;
                for (const Instruction& type : irContext->types_values()) {
                    if (IsDim1DImageType(&type)) {
                        anyDim1DLeft = true;
                        break;
                    }
                }
                if (!anyDim1DLeft) {
                    for (Instruction& capability : irContext->capabilities()) {
                        const auto value = static_cast<spv::Capability>(capability.GetSingleWordInOperand(0));
                        if (value == spv::Capability::Image1D) {
                            capability.SetInOperand(0, {static_cast<uint32_t>(spv::Capability::Shader)});
                        }
                    }
                }

                irContext->InvalidateAnalysesExceptFor(IRContext::kAnalysisNone);
                return Status::SuccessWithChange;
            }

            spvtools::Optimizer::PassToken Lower1DArrayImagesPass::CreateLower1DArrayImagesPass() {
                return spvtools::Optimizer::PassToken(spvtools::MakeUnique<Lower1DArrayImagesPass>());
            }
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
