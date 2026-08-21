// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/Lower1DSampledImagesPass.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "Lower1DSampledImagesPass.h"

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
                namespace analysis = spvtools::opt::analysis;

                // OpTypeImage in-operands: 0 sampled type, 1 Dim, 2 Depth, 3 Arrayed, 4 MS,
                // 5 Sampled, 6 Format.
                constexpr uint32_t kDimOperand = 1;
                constexpr uint32_t kArrayedOperand = 3;
                constexpr uint32_t kSampledOperand = 5;

                // Sampled == 1 is SPIR-V's "used WITH a sampler", i.e. exactly the sampler
                // uniforms this pass exists for. Sampled == 2 is the storage image
                // Lower1DArrayImagesPass owns, and Sampled == 0 ("either") is a shape glslang
                // never emits from GLSL - left out so an unexpected module is declined rather
                // than rewritten on a guess.
                bool Is1DSampledImageType(const Instruction* imageType) {
                    return imageType != nullptr && imageType->opcode() == spv::Op::OpTypeImage &&
                           imageType->NumInOperands() > kSampledOperand &&
                           static_cast<spv::Dim>(imageType->GetSingleWordInOperand(kDimOperand)) ==
                               spv::Dim::Dim1D &&
                           imageType->GetSingleWordInOperand(kSampledOperand) == 1u;
                }

                bool Is1DSampledImageTypeOfArrayedness(const Instruction* imageType, bool arrayed) {
                    return Is1DSampledImageType(imageType) &&
                           (imageType->GetSingleWordInOperand(kArrayedOperand) == 1u) == arrayed;
                }

                // Any Dim1D image still declared with Sampled == 1. Used only to decide whether
                // the Sampled1D capability is still needed after the rewrite.
                bool AnyDim1DSampledTypeLeft(IRContext* context) {
                    for (const Instruction& type : context->module()->types_values()) {
                        if (Is1DSampledImageType(&type)) return true;
                    }
                    return false;
                }

                // The OpTypeImage behind whatever an image operation was handed - a bare image, a
                // sampled image, or a pointer/array of either. Same unwrapping as
                // Lower1DArrayImagesPass, which needs the identical walk.
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
                            // whose element type is the FIRST. Both are reached here because a
                            // sampler uniform may be declared as an array of samplers.
                            type = defUseMgr->GetDef(
                                type->opcode() == spv::Op::OpTypeArray ||
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

                // How this pass classifies an opcode that can touch one of these images.
                enum class OpKind {
                    // Not an image operation at all: it may CARRY the image or sampled-image
                    // value (OpLoad, OpSampledImage, OpCopyObject, ...) but it names no
                    // coordinate, so the rewrite does not reach it.
                    NotImageOp,
                    // Addresses texels: has a coordinate at in-operand 1 and, from
                    // `imageOperandsIndex`, an optional image-operands mask.
                    Texel,
                    // Reads a property whose result does not depend on Dim. Safe to leave.
                    DimIndependentQuery,
                    // Recognised, and refused: rewriting the type would change the shape of what
                    // the shader consumes, or the operation is one this pass has no translation
                    // for.
                    Decline,
                };

                struct OpClassification {
                    OpKind kind = OpKind::NotImageOp;
                    uint32_t coordinateOperand = 1;
                    // In-operand index of the ImageOperands mask, when the opcode has one. The
                    // mask itself is OPTIONAL for the implicit-Lod, fetch and gather forms, so
                    // this is an index to test against NumInOperands(), not a promise.
                    uint32_t imageOperandsIndex = 0;
                };

                OpClassification ClassifyOpcode(spv::Op opcode) {
                    switch (opcode) {
                    // (image, coordinate, [operands]) - the mask, when present, is in-operand 2.
                    case spv::Op::OpImageSampleImplicitLod:
                    case spv::Op::OpImageSampleExplicitLod:
                    case spv::Op::OpImageSampleProjImplicitLod:
                    case spv::Op::OpImageSampleProjExplicitLod:
                    case spv::Op::OpImageFetch:
                    case spv::Op::OpImageSparseSampleImplicitLod:
                    case spv::Op::OpImageSparseSampleExplicitLod:
                    case spv::Op::OpImageSparseSampleProjImplicitLod:
                    case spv::Op::OpImageSparseSampleProjExplicitLod:
                    case spv::Op::OpImageSparseFetch:
                        return {OpKind::Texel, 1u, 2u};

                    // (image, coordinate, D_ref, [operands]) - one operand more before the mask.
                    case spv::Op::OpImageSampleDrefImplicitLod:
                    case spv::Op::OpImageSampleDrefExplicitLod:
                    case spv::Op::OpImageSampleProjDrefImplicitLod:
                    case spv::Op::OpImageSampleProjDrefExplicitLod:
                    case spv::Op::OpImageSparseSampleDrefImplicitLod:
                    case spv::Op::OpImageSparseSampleDrefExplicitLod:
                    case spv::Op::OpImageSparseSampleProjDrefImplicitLod:
                    case spv::Op::OpImageSparseSampleProjDrefExplicitLod:
                        return {OpKind::Texel, 1u, 3u};

                    // OpImageQueryLod names a coordinate and no mask. Its coordinate is the PLANE
                    // components only (no array layer), which the insert-at-1 rule widens just as
                    // correctly as a sampling coordinate.
                    case spv::Op::OpImageQueryLod:
                        return {OpKind::Texel, 1u, /*no mask*/ 0xFFFFFFFFu};

                    // Scalar result, identical for Dim1D and Dim2D.
                    case spv::Op::OpImageQueryLevels:
                        return {OpKind::DimIndependentQuery, 0u, 0u};

                    // textureSize: int for a sampler1D, ivec2 for the sampler2D it would become.
                    // There is no correct narrower answer to substitute, so the module is left
                    // alone - the sibling pass refuses the same shape for the same reason.
                    case spv::Op::OpImageQuerySize:
                    case spv::Op::OpImageQuerySizeLod:
                    // Gather is not available for 1D samplers in GLSL, so reaching one here means
                    // an input this pass did not anticipate; and its ConstOffsets operand is an
                    // ARRAY of offsets whose widening this pass does not implement.
                    case spv::Op::OpImageGather:
                    case spv::Op::OpImageDrefGather:
                    case spv::Op::OpImageSparseGather:
                    case spv::Op::OpImageSparseDrefGather:
                    // Storage-image traffic has no business reaching a Sampled == 1 image; if it
                    // does, the module is not the shape this pass reasoned about.
                    case spv::Op::OpImageRead:
                    case spv::Op::OpImageWrite:
                    case spv::Op::OpImageSparseRead:
                    case spv::Op::OpImageTexelPointer:
                    case spv::Op::OpImageQuerySamples:
                        return {OpKind::Decline, 0u, 0u};

                    default:
                        return {OpKind::NotImageOp, 0u, 0u};
                    }
                }

                // How many ids each ImageOperands bit contributes, in the bit order SPIR-V lays
                // them out in. Only the bits that carry ids need an entry; the rest contribute
                // nothing and are skipped by having a count of zero.
                struct ImageOperandBit {
                    spv::ImageOperandsMask bit;
                    uint32_t idCount;
                };
                constexpr ImageOperandBit kImageOperandBits[] = {
                    {spv::ImageOperandsMask::Bias, 1u},
                    {spv::ImageOperandsMask::Lod, 1u},
                    {spv::ImageOperandsMask::Grad, 2u},
                    {spv::ImageOperandsMask::ConstOffset, 1u},
                    {spv::ImageOperandsMask::Offset, 1u},
                    {spv::ImageOperandsMask::ConstOffsets, 1u},
                    {spv::ImageOperandsMask::Sample, 1u},
                    {spv::ImageOperandsMask::MinLod, 1u},
                    {spv::ImageOperandsMask::MakeTexelAvailable, 1u},
                    {spv::ImageOperandsMask::MakeTexelVisible, 1u},
                    {spv::ImageOperandsMask::NonPrivateTexel, 0u},
                    {spv::ImageOperandsMask::VolatileTexel, 0u},
                    {spv::ImageOperandsMask::SignExtend, 0u},
                    {spv::ImageOperandsMask::ZeroExtend, 0u},
                    {spv::ImageOperandsMask::Nontemporal, 0u},
                    {spv::ImageOperandsMask::Offsets, 1u},
                };

                // Where each of the operands this pass rewrites sits, for one instruction. An
                // index of 0 means "not present" - in-operand 0 is always the image, so it can
                // never be a real position for one of these.
                struct OperandPositions {
                    uint32_t gradX = 0;
                    uint32_t gradY = 0;
                    uint32_t constOffset = 0;
                    uint32_t offset = 0;
                    // A bit this pass does not know how to widen appeared on a covered image.
                    bool unsupported = false;

                    bool Any() const { return gradX != 0 || constOffset != 0 || offset != 0; }
                };

                OperandPositions LocateOperands(const Instruction& instruction,
                                                uint32_t imageOperandsIndex) {
                    OperandPositions positions;
                    if (imageOperandsIndex == 0xFFFFFFFFu ||
                        instruction.NumInOperands() <= imageOperandsIndex) {
                        return positions;
                    }
                    const uint32_t mask = instruction.GetSingleWordInOperand(imageOperandsIndex);
                    uint32_t next = imageOperandsIndex + 1u;
                    for (const ImageOperandBit& entry : kImageOperandBits) {
                        if ((mask & static_cast<uint32_t>(entry.bit)) == 0u) continue;
                        switch (entry.bit) {
                        case spv::ImageOperandsMask::Grad:
                            positions.gradX = next;
                            positions.gradY = next + 1u;
                            break;
                        case spv::ImageOperandsMask::ConstOffset:
                            positions.constOffset = next;
                            break;
                        case spv::ImageOperandsMask::Offset:
                            positions.offset = next;
                            break;
                        case spv::ImageOperandsMask::ConstOffsets:
                        case spv::ImageOperandsMask::Offsets:
                            // An array of offsets, only meaningful for gather - which is declined
                            // above. Refuse rather than translate half of it.
                            positions.unsupported = true;
                            break;
                        default:
                            break;
                        }
                        next += entry.idCount;
                    }
                    // Every id the mask claimed has to actually be there; a truncated operand
                    // list means the instruction is not the shape this walk assumed.
                    if (next > instruction.NumInOperands()) {
                        positions.unsupported = true;
                    }
                    return positions;
                }

                // Whether this instruction so much as mentions a value whose type resolves to a
                // covered image. Used to make sure nothing reaches these images through an opcode
                // this pass never considered: the answer decides between rewriting and declining,
                // never between two different rewrites.
                template <typename CoveredFn>
                bool MentionsCoveredImage(IRContext* context, const Instruction& instruction,
                                          const CoveredFn& covered) {
                    bool mentions = false;
                    instruction.ForEachInId([&](const uint32_t* id) {
                        if (mentions || id == nullptr) return;
                        if (covered(ResolveImageType(context, *id))) mentions = true;
                    });
                    return mentions;
                }

                // The component type of a value, and how many of them it has. A scalar reports a
                // count of 1; anything that is neither an int/float scalar nor a vector of one
                // reports 0, which every caller treats as "not a shape this pass translates".
                struct ValueShape {
                    const analysis::Type* componentType = nullptr;
                    uint32_t componentCount = 0;
                    bool IsScalar() const { return componentCount == 1u; }
                };

                ValueShape DescribeValue(IRContext* context, uint32_t valueId) {
                    ValueShape shape;
                    Instruction* def = context->get_def_use_mgr()->GetDef(valueId);
                    if (def == nullptr) return shape;
                    const analysis::Type* type = context->get_type_mgr()->GetType(def->type_id());
                    if (type == nullptr) return shape;
                    const analysis::Vector* asVector = type->AsVector();
                    const analysis::Type* component =
                        asVector != nullptr ? asVector->element_type() : type;
                    if (component == nullptr) return shape;
                    if (component->AsInteger() == nullptr && component->AsFloat() == nullptr) {
                        return shape;
                    }
                    shape.componentType = component;
                    shape.componentCount = asVector != nullptr ? asVector->element_count() : 1u;
                    return shape;
                }

                // Which 1D sampled images this module is to be rewritten for, decided per
                // arrayed-ness because that is the granularity of the OpTypeImage declarations
                // glslang emits. A category is in scope only when the module actually performs a
                // lookup on it carrying an Offset, ConstOffset or Grad - the operands SPIRV-Cross
                // prints with the wrong arity - so a shader that only samples and fetches keeps
                // SPIRV-Cross's own correct emission untouched.
                struct LoweringScope {
                    bool arrayed = false;
                    bool nonArrayed = false;

                    bool Any() const { return arrayed || nonArrayed; }
                    bool Covers(const Instruction* imageType) const {
                        return (arrayed && Is1DSampledImageTypeOfArrayedness(imageType, true)) ||
                               (nonArrayed && Is1DSampledImageTypeOfArrayedness(imageType, false));
                    }
                };

                LoweringScope ResolveLoweringScope(IRContext* context) {
                    LoweringScope scope;
                    // The type table settles the common case, and it is nearly every shader: no
                    // 1D sampled image declared at all, so the code is never walked.
                    bool declared = false;
                    for (const Instruction& type : context->module()->types_values()) {
                        if (Is1DSampledImageType(&type)) {
                            declared = true;
                            break;
                        }
                    }
                    if (!declared) return scope;

                    for (auto& function : *context->module()) {
                        for (auto& block : function) {
                            for (auto& instruction : block) {
                                const OpClassification classification =
                                    ClassifyOpcode(instruction.opcode());
                                if (classification.kind != OpKind::Texel ||
                                    instruction.NumInOperands() <= classification.coordinateOperand) {
                                    continue;
                                }
                                const Instruction* imageType =
                                    ResolveImageType(context, instruction.GetSingleWordInOperand(0));
                                if (!Is1DSampledImageType(imageType)) continue;
                                const OperandPositions positions =
                                    LocateOperands(instruction, classification.imageOperandsIndex);
                                if (!positions.Any()) continue;
                                if (imageType->GetSingleWordInOperand(kArrayedOperand) == 1u) {
                                    scope.arrayed = true;
                                } else {
                                    scope.nonArrayed = true;
                                }
                            }
                        }
                    }
                    return scope;
                }

                // Everything this pass will touch, collected before a single word is changed.
                // Planning first is what lets every refusal be a clean "leave the module alone":
                // there is no point at which the module is half converted and the pass then
                // discovers it cannot finish.
                struct RewritePlan {
                    struct Site {
                        Instruction* instruction = nullptr;
                        uint32_t coordinateOperand = 0;
                        OperandPositions operands;
                    };
                    std::vector<Site> sites;
                    bool declined = false;
                };

                RewritePlan PlanRewrite(IRContext* context, const LoweringScope& scope) {
                    RewritePlan plan;
                    const auto covered = [&scope](const Instruction* type) {
                        return scope.Covers(type);
                    };

                    for (auto& function : *context->module()) {
                        for (auto& block : function) {
                            for (auto& instruction : block) {
                                const OpClassification classification =
                                    ClassifyOpcode(instruction.opcode());

                                if (classification.kind == OpKind::NotImageOp ||
                                    classification.kind == OpKind::DimIndependentQuery) {
                                    // These name no coordinate, so they need no rewrite - but an
                                    // opcode this pass has never classified must not reach one of
                                    // these images unnoticed. NotImageOp is the catch-all, so the
                                    // check is on it.
                                    if (classification.kind == OpKind::NotImageOp &&
                                        instruction.opcode() != spv::Op::OpLoad &&
                                        instruction.opcode() != spv::Op::OpStore &&
                                        instruction.opcode() != spv::Op::OpCopyObject &&
                                        instruction.opcode() != spv::Op::OpSampledImage &&
                                        instruction.opcode() != spv::Op::OpImage &&
                                        instruction.opcode() != spv::Op::OpAccessChain &&
                                        instruction.opcode() != spv::Op::OpInBoundsAccessChain &&
                                        instruction.opcode() != spv::Op::OpPhi &&
                                        instruction.opcode() != spv::Op::OpSelect &&
                                        instruction.opcode() != spv::Op::OpFunctionCall &&
                                        MentionsCoveredImage(context, instruction, covered)) {
                                        plan.declined = true;
                                        return plan;
                                    }
                                    continue;
                                }

                                if (instruction.NumInOperands() < 1) continue;
                                const Instruction* imageType =
                                    ResolveImageType(context, instruction.GetSingleWordInOperand(0));
                                if (!scope.Covers(imageType)) continue;

                                if (classification.kind == OpKind::Decline) {
                                    plan.declined = true;
                                    return plan;
                                }
                                if (instruction.NumInOperands() <= classification.coordinateOperand) {
                                    plan.declined = true;
                                    return plan;
                                }

                                const OperandPositions positions =
                                    LocateOperands(instruction, classification.imageOperandsIndex);
                                if (positions.unsupported) {
                                    plan.declined = true;
                                    return plan;
                                }

                                // Confirm here, before anything is written, that every operand
                                // about to be widened has the shape the widening assumes. The
                                // coordinate may be a scalar or a short vector; the offset and
                                // the two gradients must be SCALARS, which for a Dim1D image is
                                // not an assumption but the validator's own rule
                                // (GetPlaneCoordSize(1D) == 1). Checking it up front is what
                                // keeps the apply phase total.
                                const ValueShape coordinate = DescribeValue(
                                    context, instruction.GetSingleWordInOperand(
                                                 classification.coordinateOperand));
                                if (coordinate.componentCount == 0u || coordinate.componentCount > 3u) {
                                    plan.declined = true;
                                    return plan;
                                }
                                const uint32_t scalarOperands[] = {positions.gradX, positions.gradY,
                                                                   positions.offset,
                                                                   positions.constOffset};
                                for (const uint32_t position : scalarOperands) {
                                    if (position == 0u) continue;
                                    if (!DescribeValue(context,
                                                       instruction.GetSingleWordInOperand(position))
                                             .IsScalar()) {
                                        plan.declined = true;
                                        return plan;
                                    }
                                }
                                // ConstOffset has to stay a constant expression, so its widened
                                // form is built as a module-scope constant - which is only
                                // possible if the operand really is one.
                                if (positions.constOffset != 0u &&
                                    context->get_constant_mgr()->FindDeclaredConstant(
                                        instruction.GetSingleWordInOperand(positions.constOffset)) ==
                                        nullptr) {
                                    plan.declined = true;
                                    return plan;
                                }

                                plan.sites.push_back(
                                    {&instruction, classification.coordinateOperand, positions});
                            }
                        }
                    }
                    return plan;
                }
            } // namespace

            bool Lower1DSampledImagesPass::BinaryHasOffsetOrGrad1DSampledImage(
                const Vector<Uint32>& binary) {
                if (binary.empty()) {
                    return false;
                }
                std::unique_ptr<IRContext> context = spvtools::BuildModule(
                    SPV_ENV_VULKAN_1_1,
                    [](spv_message_level_t, const char*, const spv_position_t&, const char*) {},
                    binary.data(), binary.size());
                if (!context) {
                    return false;
                }
                return ResolveLoweringScope(context.get()).Any();
            }

            spvtools::opt::Pass::Status Lower1DSampledImagesPass::Process() {
                auto* irContext = context();
                auto* typeMgr = irContext->get_type_mgr();
                auto* constantMgr = irContext->get_constant_mgr();

                const LoweringScope scope = ResolveLoweringScope(irContext);
                if (!scope.Any()) {
                    return Status::SuccessWithoutChange;
                }

                RewritePlan plan = PlanRewrite(irContext, scope);
                if (plan.declined) {
                    return Status::SuccessWithoutChange;
                }

                // A zero of a given 32-bit scalar type. The literal word is the VALUE's bit
                // pattern, which for a float zero is 0 as well - so one helper serves the integer
                // coordinate of a fetch, the float coordinate of a sample and the float gradients
                // alike, without a second spelling to keep in step.
                const auto zeroOf = [&](const analysis::Type* componentType,
                                        uint32_t componentTypeId) -> uint32_t {
                    const analysis::Constant* constant =
                        constantMgr->GetConstant(componentType, {0u});
                    if (constant == nullptr) return 0u;
                    const Instruction* defining =
                        constantMgr->GetDefiningInstruction(constant, componentTypeId);
                    return defining != nullptr ? defining->result_id() : 0u;
                };

                // The whole of the arity repair, in one place: insert a zero at component 1.
                // Scalar u becomes (u, 0); (u, layer) becomes (u, 0, layer); (u, q) becomes
                // (u, 0, q). See the header for why one rule covers every shape.
                const auto widen = [&](uint32_t valueId, Instruction* before,
                                       bool mustBeConstant) -> uint32_t {
                    const ValueShape shape = DescribeValue(irContext, valueId);
                    if (shape.componentCount == 0u) return 0u;

                    const uint32_t componentTypeId = typeMgr->GetTypeInstruction(shape.componentType);
                    if (componentTypeId == 0u) return 0u;
                    analysis::Vector widenedCandidate(shape.componentType, shape.componentCount + 1u);
                    const uint32_t widenedTypeId = typeMgr->GetTypeInstruction(&widenedCandidate);
                    const uint32_t zeroId = zeroOf(shape.componentType, componentTypeId);
                    if (widenedTypeId == 0u || zeroId == 0u) return 0u;

                    // ConstOffset must remain a constant expression - the validator says so
                    // outright ("Expected Image Operand ConstOffset to be a const object") - so
                    // for it the widened value is built as a module-scope OpConstantComposite
                    // rather than as an instruction in the block. Only the scalar shape is
                    // reachable: the plan phase refuses anything else, because a Dim1D image's
                    // offset has exactly one component by the validator's own arity rule.
                    if (mustBeConstant) {
                        if (!shape.IsScalar()) return 0u;
                        const analysis::Type* widenedType = typeMgr->GetType(widenedTypeId);
                        const analysis::Constant* widenedConstant =
                            widenedType != nullptr
                                ? constantMgr->GetConstant(widenedType, {valueId, zeroId})
                                : nullptr;
                        if (widenedConstant == nullptr) return 0u;
                        const Instruction* defining =
                            constantMgr->GetDefiningInstruction(widenedConstant, widenedTypeId);
                        return defining != nullptr ? defining->result_id() : 0u;
                    }

                    InstructionBuilder builder(
                        irContext, before,
                        IRContext::kAnalysisDefUse | IRContext::kAnalysisInstrToBlockMapping);
                    std::vector<uint32_t> componentIds;
                    componentIds.reserve(shape.componentCount + 1u);
                    if (shape.IsScalar()) {
                        componentIds.push_back(valueId);
                        componentIds.push_back(zeroId);
                    } else {
                        for (uint32_t i = 0; i < shape.componentCount; ++i) {
                            Instruction* extracted =
                                builder.AddCompositeExtract(componentTypeId, valueId, {i});
                            if (extracted == nullptr) return 0u;
                            componentIds.push_back(extracted->result_id());
                            if (i == 0u) componentIds.push_back(zeroId);
                        }
                    }
                    Instruction* widened =
                        builder.AddCompositeConstruct(widenedTypeId, componentIds);
                    return widened != nullptr ? widened->result_id() : 0u;
                };

                for (RewritePlan::Site& site : plan.sites) {
                    Instruction* instruction = site.instruction;

                    struct Target {
                        uint32_t position;
                        bool mustBeConstant;
                    };
                    const Target targets[] = {
                        {site.coordinateOperand, false},
                        {site.operands.gradX, false},
                        {site.operands.gradY, false},
                        {site.operands.offset, false},
                        {site.operands.constOffset, true},
                    };
                    for (const Target& target : targets) {
                        // Position 0 is the image operand, so it is this plan's "absent" marker
                        // for everything except the coordinate, which is never 0.
                        if (target.position == 0u) continue;
                        const uint32_t widenedId =
                            widen(instruction->GetSingleWordInOperand(target.position), instruction,
                                  target.mustBeConstant);
                        if (widenedId == 0u) {
                            // Reachable only if the module's shapes disagree with what the plan
                            // recorded. Failing here makes the caller keep the input binary,
                            // which is the same outcome as a decline.
                            return Status::Failure;
                        }
                        instruction->SetInOperand(target.position, {widenedId});
                    }
                    irContext->UpdateDefUse(instruction);
                }

                // Only now, with no lookup still spelling a 1D coordinate, does the type become
                // the 2D one - which is what ES stores a GL_TEXTURE_1D(_ARRAY) as anyway
                // (MapToBackendTextureTarget), and what SPIRV-Cross was already PRINTING for it.
                for (Instruction& type : irContext->types_values()) {
                    if (scope.Covers(&type)) {
                        type.SetInOperand(kDimOperand, {static_cast<uint32_t>(spv::Dim::Dim2D)});
                    }
                }

                // Sampled1D describes the types just rewritten. Drop it only if no 1D SAMPLED
                // image is left at all - a module may still hold one this pass left alone (a
                // category with no offset or gradient on it), and that one still needs the
                // capability. Image1D is deliberately untouched: it belongs to the storage images
                // Lower1DArrayImagesPass owns, and they may still be Dim1D here. Shader is
                // declared by any module reaching this point, so restating it keeps the
                // instruction valid and RemoveDuplicates collapses the pair.
                if (!AnyDim1DSampledTypeLeft(irContext)) {
                    for (Instruction& capability : irContext->capabilities()) {
                        const auto value =
                            static_cast<spv::Capability>(capability.GetSingleWordInOperand(0));
                        if (value == spv::Capability::Sampled1D) {
                            capability.SetInOperand(0, {static_cast<uint32_t>(spv::Capability::Shader)});
                        }
                    }
                }

                irContext->InvalidateAnalysesExceptFor(IRContext::kAnalysisNone);
                return Status::SuccessWithChange;
            }

            spvtools::Optimizer::PassToken Lower1DSampledImagesPass::CreateLower1DSampledImagesPass() {
                return spvtools::Optimizer::PassToken(
                    spvtools::MakeUnique<Lower1DSampledImagesPass>());
            }
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
