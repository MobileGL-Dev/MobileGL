// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/ClampMultisampleFetchPass.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "ClampMultisampleFetchPass.h"

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
#include "source/util/string_utils.h"

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

                // GLSL.std.450 instruction numbers (see 3rdparty/glslang/SPIRV/GLSL.std.450.h).
                // UMin is not interchangeable with SMin here: an unsigned operand large enough to
                // read as negative would come back OUT of range from a signed minimum, which is
                // the very thing this pass exists to prevent.
                constexpr uint32_t kGlslUMin = 38u;
                constexpr uint32_t kGlslSMin = 39u;

                // OpTypeImage in-operands: 0 sampled type, 1 Dim, 2 Depth, 3 Arrayed, 4 MS,
                // 5 Sampled, 6 Format.
                constexpr uint32_t kSampledTypeOperand = 0;
                constexpr uint32_t kDepthOperand = 2;
                constexpr uint32_t kMultisampledOperand = 4;

                // OpImageFetch / OpImageRead in-operands: 0 image, 1 coordinate, 2 the optional
                // image-operands mask, 3.. the ids that mask asks for.
                constexpr uint32_t kImageOperand = 0;
                constexpr uint32_t kImageOperandsMaskOperand = 2;

                // The categories GL keeps a separate GL_MAX_*_SAMPLES ceiling for.
                enum class SampleCategory { Color, Depth, Integer };

                // The two operations that can carry a Sample image-operand and take their
                // coordinate in in-operand 1. OpImageWrite can carry one too, but its operand
                // layout differs (image, coordinate, TEXEL, mask) and writing a multisample
                // texel is not expressible in the ESSL this backend emits, so it is left out
                // rather than given an untested second index arithmetic. The sparse forms are
                // out of scope for the same reason: ESSL has no sparse texturing at all, so a
                // module containing one cannot reach a driver through this path anyway.
                bool CarriesSampleImageOperand(spv::Op opcode) {
                    return opcode == spv::Op::OpImageFetch || opcode == spv::Op::OpImageRead;
                }

                bool IsMultisampledImageType(const Instruction* imageType) {
                    return imageType != nullptr && imageType->opcode() == spv::Op::OpTypeImage &&
                           imageType->NumInOperands() > kMultisampledOperand &&
                           imageType->GetSingleWordInOperand(kMultisampledOperand) == 1u;
                }

                // The OpTypeImage behind whatever an image operation was handed - a sampled
                // image, a bare image, or a pointer to (or array of) either. Same unwrapping as
                // Lower1DArrayImagesPass.
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
                            // whose element type is the FIRST.
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

                SampleCategory CategoryOf(IRContext* context, const Instruction* imageType) {
                    const Instruction* sampledType =
                        context->get_def_use_mgr()->GetDef(imageType->GetSingleWordInOperand(kSampledTypeOperand));
                    if (sampledType != nullptr && sampledType->opcode() == spv::Op::OpTypeInt) {
                        return SampleCategory::Integer;
                    }
                    // Depth == 1 is the ONLY spelling that positively means a depth image.
                    // glslang writes 0 for a plain sampler and 2 ("no indication") wherever it
                    // cannot tell, and GLSL has no multisampled shadow sampler for it to write 1
                    // for, so everything but an explicit 1 falls to the colour ceiling - which is
                    // also the safer of the two to guess at, being the one GL_MAX_SAMPLES itself
                    // describes. Guarded because Depth is only readable on a well-formed type.
                    if (imageType->NumInOperands() > kDepthOperand &&
                        imageType->GetSingleWordInOperand(kDepthOperand) == 1u) {
                        return SampleCategory::Depth;
                    }
                    return SampleCategory::Color;
                }

                // Where the Sample id sits among an image operation's in-operands, or false when
                // the operation carries no Sample at all.
                //
                // The position is NOT fixed. The mask's ids follow it in ASCENDING BIT ORDER, so
                // every lower bit that is set pushes Sample along by the number of ids that bit
                // asks for: Bias/Lod/ConstOffset/Offset/ConstOffsets one each, Grad two (dx and
                // dy). Bits at or above Sample cannot move it and are irrelevant here. glslang
                // only ever emits Sample on its own for a GLSL texelFetch - there is no
                // texelFetchOffset for a multisampled sampler - so in practice this always
                // answers 3; the walk is what keeps that from being an assumption.
                bool TryGetSampleOperandIndex(const Instruction& instruction, uint32_t* sampleOperandIndex) {
                    if (instruction.NumInOperands() <= kImageOperandsMaskOperand) {
                        // No image-operands mask at all, so no explicit sample: SPIR-V reads
                        // sample 0, which is in range of any allocation. Nothing to clamp.
                        return false;
                    }
                    const uint32_t mask = instruction.GetSingleWordInOperand(kImageOperandsMaskOperand);
                    const auto has = [mask](spv::ImageOperandsMask bit) {
                        return (mask & static_cast<uint32_t>(bit)) != 0u;
                    };
                    if (!has(spv::ImageOperandsMask::Sample)) {
                        return false;
                    }

                    uint32_t index = kImageOperandsMaskOperand + 1;
                    if (has(spv::ImageOperandsMask::Bias)) ++index;
                    if (has(spv::ImageOperandsMask::Lod)) ++index;
                    if (has(spv::ImageOperandsMask::Grad)) index += 2;
                    if (has(spv::ImageOperandsMask::ConstOffset)) ++index;
                    if (has(spv::ImageOperandsMask::Offset)) ++index;
                    if (has(spv::ImageOperandsMask::ConstOffsets)) ++index;
                    if (instruction.NumInOperands() <= index) {
                        // A mask promising more operands than the instruction carries is a
                        // malformed module; leave it to the validator rather than indexing past
                        // the end of it.
                        return false;
                    }
                    *sampleOperandIndex = index;
                    return true;
                }

                // The module's GLSL.std.450 import, creating it when the module has none.
                // glslang emits one for all but the most trivial shaders, but a module that
                // reached here without one must still be clampable. 0 means no id was available,
                // and in that case NOTHING was added - the caller can still leave the module
                // untouched. IRContext::AddExtInstImport rather than Module's: it is the one that
                // keeps the def-use and feature managers in step with the new import.
                uint32_t EnsureGlslStd450Import(IRContext* context) {
                    for (const Instruction& import : context->module()->ext_inst_imports()) {
                        if (spvtools::utils::MakeString(import.GetInOperand(0).words) == "GLSL.std.450") {
                            return import.result_id();
                        }
                    }
                    const uint32_t importId = context->TakeNextId();
                    if (importId == 0u) return 0u;
                    context->AddExtInstImport(spvtools::MakeUnique<Instruction>(
                        context, spv::Op::OpExtInstImport, 0, importId,
                        Instruction::OperandList{
                            {SPV_OPERAND_TYPE_LITERAL_STRING, spvtools::utils::MakeVector("GLSL.std.450")}}));
                    return importId;
                }
            } // namespace

            bool ClampMultisampleFetchPass::DeclaresMultisampledImage(const Vector<Uint32>& binary) {
                if (binary.empty()) {
                    // An empty module is a stage that produced no SPIR-V, which is not a verdict
                    // about multisample fetches; letting BuildModule reject it would push a
                    // spurious diagnostic through the message consumer first.
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
                for (const Instruction& type : context->module()->types_values()) {
                    if (IsMultisampledImageType(&type)) {
                        return true;
                    }
                }
                return false;
            }

            spvtools::opt::Pass::Status ClampMultisampleFetchPass::Process() {
                // No category is squeezed, so no fetch can be out of range. This is the whole
                // answer on a driver whose per-format ceilings all reach what MobileGL
                // advertises, and it costs nothing.
                if (m_maxColorSamples >= m_advertisedMaxSamples &&
                    m_maxIntegerSamples >= m_advertisedMaxSamples &&
                    m_maxDepthSamples >= m_advertisedMaxSamples) {
                    return Status::SuccessWithoutChange;
                }

                auto* irContext = context();

                // The type table settles it for almost every shader: no multisampled image
                // declared, nothing any fetch in the body could be reading.
                bool hasMultisampledImageType = false;
                for (const Instruction& type : irContext->types_values()) {
                    if (IsMultisampledImageType(&type)) {
                        hasMultisampledImageType = true;
                        break;
                    }
                }
                if (!hasMultisampledImageType) {
                    return Status::SuccessWithoutChange;
                }

                auto* defUseMgr = irContext->get_def_use_mgr();
                auto* typeMgr = irContext->get_type_mgr();
                auto* constantMgr = irContext->get_constant_mgr();

                bool clampedAnything = false;
                for (auto& function : *irContext->module()) {
                    for (auto& block : function) {
                        for (auto& instruction : block) {
                            if (!CarriesSampleImageOperand(instruction.opcode()) ||
                                instruction.NumInOperands() <= kImageOperandsMaskOperand) {
                                continue;
                            }
                            const Instruction* imageType =
                                ResolveImageType(irContext, instruction.GetSingleWordInOperand(kImageOperand));
                            if (!IsMultisampledImageType(imageType)) {
                                continue;
                            }
                            uint32_t sampleOperandIndex = 0;
                            if (!TryGetSampleOperandIndex(instruction, &sampleOperandIndex)) {
                                continue;
                            }

                            Int32 categoryMaxSamples = m_maxColorSamples;
                            switch (CategoryOf(irContext, imageType)) {
                            case SampleCategory::Integer:
                                categoryMaxSamples = m_maxIntegerSamples;
                                break;
                            case SampleCategory::Depth:
                                categoryMaxSamples = m_maxDepthSamples;
                                break;
                            case SampleCategory::Color:
                                break;
                            }
                            if (categoryMaxSamples >= m_advertisedMaxSamples) {
                                continue;
                            }

                            // The replacement has to carry the ORIGINAL operand's type: SPIR-V
                            // permits either signedness for Sample, and handing OpImageFetch an
                            // int where it had a uint is an invalid module rather than a wrong
                            // answer - the kind of defect that reaches a driver as "compiles
                            // here, not there".
                            const uint32_t sampleOperandId = instruction.GetSingleWordInOperand(sampleOperandIndex);
                            const Instruction* sampleOperandDef = defUseMgr->GetDef(sampleOperandId);
                            if (sampleOperandDef == nullptr) {
                                continue;
                            }
                            const uint32_t sampleTypeId = sampleOperandDef->type_id();
                            const analysis::Type* sampleType =
                                sampleTypeId != 0u ? typeMgr->GetType(sampleTypeId) : nullptr;
                            const analysis::Integer* sampleInteger =
                                sampleType != nullptr ? sampleType->AsInteger() : nullptr;
                            if (sampleInteger == nullptr || sampleInteger->width() != 32u) {
                                // GLSL spells the sample index `int` and SPIR-V requires an
                                // integer scalar, so this is unreachable from any shader this
                                // backend compiles. Declining beats minting a constant of a
                                // width the operand never had.
                                MGLOG_D("ClampMultisampleFetchPass: sample operand %%%u of a "
                                        "multisample fetch is not a 32-bit integer scalar; left "
                                        "unclamped.",
                                        sampleOperandId);
                                continue;
                            }

                            if (categoryMaxSamples <= 1) {
                                // One sample exists, and its index is 0.
                                const analysis::Constant* zero = constantMgr->GetConstant(sampleType, {0u});
                                const Instruction* zeroInst =
                                    zero != nullptr ? constantMgr->GetDefiningInstruction(zero, sampleTypeId)
                                                    : nullptr;
                                if (zeroInst == nullptr) {
                                    return Status::Failure;
                                }
                                instruction.SetInOperand(sampleOperandIndex, {zeroInst->result_id()});
                                irContext->UpdateDefUse(&instruction);
                                clampedAnything = true;
                                continue;
                            }

                            // min(operand, K-1). Only the upper bound: an index already inside
                            // the allocation comes through untouched, which is what makes this
                            // safe to apply to a shader that was already correct.
                            //
                            // Everything from here on either completes or fails the module.
                            // Anything that gives up half way - after the import or the bound
                            // constant has been added - would leave a MUTATED module reported as
                            // SuccessWithoutChange, which spvtools::Optimizer asserts against
                            // (it re-serialises and compares byte for byte in that case).
                            const uint32_t glslStd450Id = EnsureGlslStd450Import(irContext);
                            if (glslStd450Id == 0u) {
                                // Id space exhausted, and the import was NOT added. Nothing has
                                // changed yet, but nothing further can be built either.
                                return Status::Failure;
                            }
                            const uint32_t resultId = irContext->TakeNextId();
                            const analysis::Constant* bound = constantMgr->GetConstant(
                                sampleType, {static_cast<uint32_t>(categoryMaxSamples - 1)});
                            const Instruction* boundInst =
                                bound != nullptr ? constantMgr->GetDefiningInstruction(bound, sampleTypeId)
                                                 : nullptr;
                            if (resultId == 0u || boundInst == nullptr) {
                                return Status::Failure;
                            }

                            InstructionBuilder builder(
                                irContext, &instruction,
                                IRContext::kAnalysisDefUse | IRContext::kAnalysisInstrToBlockMapping);
                            Instruction* clamped = builder.AddInstruction(spvtools::MakeUnique<Instruction>(
                                irContext, spv::Op::OpExtInst, sampleTypeId, resultId,
                                Instruction::OperandList{
                                    {SPV_OPERAND_TYPE_ID, {glslStd450Id}},
                                    {SPV_OPERAND_TYPE_EXTENSION_INSTRUCTION_NUMBER,
                                     {sampleInteger->IsSigned() ? kGlslSMin : kGlslUMin}},
                                    {SPV_OPERAND_TYPE_ID, {sampleOperandId}},
                                    {SPV_OPERAND_TYPE_ID, {boundInst->result_id()}}}));
                            if (clamped == nullptr) {
                                return Status::Failure;
                            }
                            instruction.SetInOperand(sampleOperandIndex, {clamped->result_id()});
                            irContext->UpdateDefUse(&instruction);
                            clampedAnything = true;
                        }
                    }
                }

                if (!clampedAnything) {
                    return Status::SuccessWithoutChange;
                }
                irContext->InvalidateAnalysesExceptFor(IRContext::kAnalysisNone);
                return Status::SuccessWithChange;
            }

            spvtools::Optimizer::PassToken ClampMultisampleFetchPass::CreateClampMultisampleFetchPass(
                const Int32 maxColorSamples, const Int32 maxIntegerSamples, const Int32 maxDepthSamples,
                const Int32 advertisedMaxSamples) {
                return spvtools::Optimizer::PassToken(spvtools::MakeUnique<ClampMultisampleFetchPass>(
                    maxColorSamples, maxIntegerSamples, maxDepthSamples, advertisedMaxSamples));
            }
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
