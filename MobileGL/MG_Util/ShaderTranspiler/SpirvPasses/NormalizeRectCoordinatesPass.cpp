// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/NormalizeRectCoordinatesPass.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "NormalizeRectCoordinatesPass.h"

#include "spirv.hpp"
#include "source/opt/constants.h"
#include "source/opt/def_use_manager.h"
#include "source/opt/instruction.h"
#include "source/opt/ir_builder.h"
#include "source/opt/ir_context.h"
#include "source/opt/module.h"
#include "source/opt/type_manager.h"
#include "source/opt/types.h"
#include "source/util/make_unique.h"

#include <unordered_set>
#include <vector>

namespace MobileGL {
    namespace MG_Util {
        namespace ShaderTranspiler {
            namespace {
                using spvtools::opt::Instruction;
                using spvtools::opt::IRContext;
                using spvtools::opt::InstructionBuilder;

                // Image operations whose coordinate operand (in-operand 1) is a plain
                // normalized coordinate and nothing else. The Dref *sample* forms are absent:
                // they pack the compare value into the coordinate's last component, so the
                // divide cannot be applied componentwise. OpImageDrefGather is here because it
                // carries the compare value in a separate operand.
                bool TakesPlainNormalizedCoordinate(spv::Op opcode) {
                    switch (opcode) {
                    case spv::Op::OpImageSampleImplicitLod:
                    case spv::Op::OpImageSampleExplicitLod:
                    case spv::Op::OpImageGather:
                    case spv::Op::OpImageDrefGather:
                        return true;
                    default:
                        return false;
                    }
                }

                // The OpTypeImage behind whatever an image operation was handed - a sampled
                // image, a bare image, or a pointer to either. Returns nullptr when the operand
                // is not an image at all.
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
                            // Both name their element type in their last in-operand.
                            type = defUseMgr->GetDef(type->GetSingleWordInOperand(type->NumInOperands() - 1));
                            continue;
                        default:
                            return nullptr;
                        }
                    }
                    return nullptr;
                }

                bool IsRectImageType(const Instruction* imageType) {
                    // OpTypeImage in-operands: sampled type, Dim, Depth, Arrayed, MS, Sampled, Format.
                    return imageType != nullptr && imageType->NumInOperands() >= 2 &&
                           static_cast<spv::Dim>(imageType->GetSingleWordInOperand(1)) == spv::Dim::Rect;
                }

                // The bare image an OpImageQuerySizeLod needs. An operation on a sampled image
                // has to unwrap it first; one already holding a bare image is used as is.
                uint32_t GetQueryableImage(IRContext* context, InstructionBuilder& builder, uint32_t imageOperandId,
                                         uint32_t imageTypeId) {
                    auto* defUseMgr = context->get_def_use_mgr();
                    Instruction* object = defUseMgr->GetDef(imageOperandId);
                    if (object == nullptr) return 0;
                    Instruction* type = defUseMgr->GetDef(object->type_id());
                    if (type != nullptr && type->opcode() == spv::Op::OpTypeImage) {
                        return imageOperandId;
                    }
                    Instruction* unwrapped = builder.AddUnaryOp(imageTypeId, spv::Op::OpImage, imageOperandId);
                    return unwrapped != nullptr ? unwrapped->result_id() : 0;
                }
            } // namespace

            spvtools::opt::Pass::Status NormalizeRectCoordinatesPass::Process() {
                auto* irContext = context();
                auto* typeMgr = irContext->get_type_mgr();
                auto* constantMgr = irContext->get_constant_mgr();

                // Nothing to do unless the module actually declares a rectangle image.
                bool hasRectImageType = false;
                for (const Instruction& type : irContext->types_values()) {
                    if (type.opcode() == spv::Op::OpTypeImage && IsRectImageType(&type)) {
                        hasRectImageType = true;
                        break;
                    }
                }
                if (!hasRectImageType) {
                    return Status::SuccessWithoutChange;
                }

                spvtools::opt::analysis::Integer signedInt(32, true);
                spvtools::opt::analysis::Float float32(32);
                spvtools::opt::analysis::Vector int2(&signedInt, 2);
                spvtools::opt::analysis::Vector float2(&float32, 2);
                const uint32_t int2TypeId = typeMgr->GetTypeInstruction(&int2);
                const uint32_t float2TypeId = typeMgr->GetTypeInstruction(&float2);
                const uint32_t lodZeroId = constantMgr->GetSIntConstId(0);
                if (int2TypeId == 0 || float2TypeId == 0 || lodZeroId == 0) {
                    return Status::Failure;
                }

                bool rewroteCoordinate = false;
                for (auto& function : *irContext->module()) {
                    for (auto& block : function) {
                        for (auto& instruction : block) {
                            if (!TakesPlainNormalizedCoordinate(instruction.opcode()) ||
                                instruction.NumInOperands() < 2) {
                                continue;
                            }
                            const uint32_t imageOperandId = instruction.GetSingleWordInOperand(0);
                            Instruction* imageType = ResolveImageType(irContext, imageOperandId);
                            if (!IsRectImageType(imageType)) {
                                continue;
                            }

                            const uint32_t coordinateId = instruction.GetSingleWordInOperand(1);
                            InstructionBuilder builder(
                                irContext, &instruction,
                                IRContext::kAnalysisDefUse | IRContext::kAnalysisInstrToBlockMapping);
                            const uint32_t queryableImageId =
                                GetQueryableImage(irContext, builder, imageOperandId, imageType->result_id());
                            if (queryableImageId == 0) {
                                return Status::Failure;
                            }

                            // A rectangle image has exactly one level, so the query's level is 0.
                            // The lod form is what the 2D type this becomes accepts.
                            Instruction* size = builder.AddBinaryOp(int2TypeId, spv::Op::OpImageQuerySizeLod,
                                                                    queryableImageId, lodZeroId);
                            Instruction* sizeFloat =
                                builder.AddUnaryOp(float2TypeId, spv::Op::OpConvertSToF, size->result_id());
                            Instruction* normalized = builder.AddBinaryOp(
                                float2TypeId, spv::Op::OpFDiv, coordinateId, sizeFloat->result_id());
                            instruction.SetInOperand(1, {normalized->result_id()});
                            irContext->UpdateDefUse(&instruction);
                            rewroteCoordinate = true;
                        }
                    }
                }

                // Now that no lookup depends on the rectangle semantics any more, the type can
                // become the 2D one both targets accept. Done unconditionally, because a module
                // that only ever fetched texels still has to lose the type.
                for (Instruction& type : irContext->types_values()) {
                    if (type.opcode() == spv::Op::OpTypeImage && IsRectImageType(&type)) {
                        type.SetInOperand(1, {static_cast<uint32_t>(spv::Dim::Dim2D)});
                    }
                }
                // The rectangle capabilities describe types that no longer exist. Shader is
                // always declared by a graphics module, so restating it keeps the instruction
                // valid without leaving a capability a consumer would key off.
                for (Instruction& capability : irContext->capabilities()) {
                    const auto value = static_cast<spv::Capability>(capability.GetSingleWordInOperand(0));
                    if (value == spv::Capability::SampledRect || value == spv::Capability::ImageRect) {
                        capability.SetInOperand(0, {static_cast<uint32_t>(spv::Capability::Shader)});
                    }
                }
                if (rewroteCoordinate) {
                    irContext->AddCapability(spv::Capability::ImageQuery);
                }

                irContext->InvalidateAnalysesExceptFor(IRContext::kAnalysisNone);
                return Status::SuccessWithChange;
            }

            spvtools::Optimizer::PassToken NormalizeRectCoordinatesPass::CreateNormalizeRectCoordinatesPass() {
                return spvtools::Optimizer::PassToken(spvtools::MakeUnique<NormalizeRectCoordinatesPass>());
            }
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
