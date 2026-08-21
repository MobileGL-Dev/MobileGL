// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/WidenImageFormatsPass.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "WidenImageFormatsPass.h"

#include "spirv.hpp"
#include "source/opt/build_module.h"
#include "source/opt/constants.h"
#include "source/opt/def_use_manager.h"
#include "source/opt/instruction.h"
#include "source/opt/ir_context.h"
#include "source/opt/module.h"
#include "source/opt/type_manager.h"
#include "source/opt/types.h"
#include "source/util/make_unique.h"

#include <map>
#include <memory>
#include <vector>

namespace MobileGL {
    namespace MG_Util {
        namespace ShaderTranspiler {
            namespace {
                using spvtools::opt::Instruction;
                using spvtools::opt::IRContext;
                using spvtools::opt::Operand;
                namespace analysis = spvtools::opt::analysis;

                // OpTypeImage in-operands: 0 sampled type, 1 Dim, 2 Depth, 3 Arrayed, 4 MS,
                // 5 Sampled, 6 Format.
                constexpr uint32_t kImageSampledTypeOperand = 0;
                constexpr uint32_t kImageSampledOperand = 5;
                constexpr uint32_t kImageFormatOperand = 6;
                // A storage image, i.e. one reached through imageLoad/imageStore rather than a
                // sampler. The only kind that carries a format qualifier in any GLSL dialect.
                constexpr uint32_t kSampledStorageImage = 2;

                // OpImageRead in-operands: 0 image, 1 coordinate, 2.. optional image operands.
                // OpImageWrite in-operands: 0 image, 1 coordinate, 2 texel, 3.. optional.
                constexpr uint32_t kImageAccessImageOperand = 0;
                constexpr uint32_t kImageWriteTexelOperand = 2;

                // The exact carrier of a non-core image format: the core GLSL ES format with the
                // SAME component type and the SAME per-channel width, differing only in channel
                // count. `channels` is what the original format really has, which is what every
                // access through the carrier is masked back to.
                //
                // Only formats that widen EXACTLY appear here. r11f_g11f_b10f, rgb10_a2,
                // rgb10_a2ui, rgba16, rg16, r16, rgba16_snorm, rg16_snorm and r16_snorm have no
                // same-width core carrier - every candidate is either lossy or changes the numeric
                // domain a sampler would read - and are deliberately absent, so they keep the
                // honest "no GLSL ES spelling" diagnostic rather than a silent approximation.
                struct ImageFormatWidening {
                    spv::ImageFormat Carrier = spv::ImageFormat::Unknown;
                    uint32_t Channels = 0;

                    explicit operator bool() const { return Carrier != spv::ImageFormat::Unknown; }
                };

                ImageFormatWidening WideningOfSpirvImageFormat(spv::ImageFormat format) {
                    switch (format) {
                    // Float.
                    case spv::ImageFormat::Rg32f: return {spv::ImageFormat::Rgba32f, 2};
                    case spv::ImageFormat::Rg16f: return {spv::ImageFormat::Rgba16f, 2};
                    case spv::ImageFormat::R16f: return {spv::ImageFormat::Rgba16f, 1};
                    // Unsigned normalized.
                    case spv::ImageFormat::Rg8: return {spv::ImageFormat::Rgba8, 2};
                    case spv::ImageFormat::R8: return {spv::ImageFormat::Rgba8, 1};
                    // Signed normalized.
                    case spv::ImageFormat::Rg8Snorm: return {spv::ImageFormat::Rgba8Snorm, 2};
                    case spv::ImageFormat::R8Snorm: return {spv::ImageFormat::Rgba8Snorm, 1};
                    // Signed integer.
                    case spv::ImageFormat::Rg32i: return {spv::ImageFormat::Rgba32i, 2};
                    case spv::ImageFormat::Rg16i: return {spv::ImageFormat::Rgba16i, 2};
                    case spv::ImageFormat::R16i: return {spv::ImageFormat::Rgba16i, 1};
                    case spv::ImageFormat::Rg8i: return {spv::ImageFormat::Rgba8i, 2};
                    case spv::ImageFormat::R8i: return {spv::ImageFormat::Rgba8i, 1};
                    // Unsigned integer.
                    case spv::ImageFormat::Rg32ui: return {spv::ImageFormat::Rgba32ui, 2};
                    case spv::ImageFormat::Rg16ui: return {spv::ImageFormat::Rgba16ui, 2};
                    case spv::ImageFormat::R16ui: return {spv::ImageFormat::Rgba16ui, 1};
                    case spv::ImageFormat::Rg8ui: return {spv::ImageFormat::Rgba8ui, 2};
                    case spv::ImageFormat::R8ui: return {spv::ImageFormat::Rgba8ui, 1};
                    default:
                        return {};
                    }
                }

                // The GL 4.2 image format table (core spec table 8.26) as SPIR-V ImageFormats.
                // Written as literals rather than through the GL headers because this lives in
                // MG_Util, which the GL frontend's enums do not reach; the same list, in the same
                // order, as BakeImageFormatsPass::SpirvImageFormatFromGLInternalFormat.
                spv::ImageFormat SpirvImageFormatOfGL(Uint glInternalFormat) {
                    switch (glInternalFormat) {
                    case 0x8814: /*GL_RGBA32F*/ return spv::ImageFormat::Rgba32f;
                    case 0x881A: /*GL_RGBA16F*/ return spv::ImageFormat::Rgba16f;
                    case 0x8230: /*GL_RG32F*/ return spv::ImageFormat::Rg32f;
                    case 0x822F: /*GL_RG16F*/ return spv::ImageFormat::Rg16f;
                    case 0x8C3A: /*GL_R11F_G11F_B10F*/ return spv::ImageFormat::R11fG11fB10f;
                    case 0x822E: /*GL_R32F*/ return spv::ImageFormat::R32f;
                    case 0x822D: /*GL_R16F*/ return spv::ImageFormat::R16f;
                    case 0x8D70: /*GL_RGBA32UI*/ return spv::ImageFormat::Rgba32ui;
                    case 0x8D76: /*GL_RGBA16UI*/ return spv::ImageFormat::Rgba16ui;
                    case 0x8D7C: /*GL_RGBA8UI*/ return spv::ImageFormat::Rgba8ui;
                    case 0x906F: /*GL_RGB10_A2UI*/ return spv::ImageFormat::Rgb10a2ui;
                    case 0x823C: /*GL_RG32UI*/ return spv::ImageFormat::Rg32ui;
                    case 0x823A: /*GL_RG16UI*/ return spv::ImageFormat::Rg16ui;
                    case 0x8238: /*GL_RG8UI*/ return spv::ImageFormat::Rg8ui;
                    case 0x8236: /*GL_R32UI*/ return spv::ImageFormat::R32ui;
                    case 0x8234: /*GL_R16UI*/ return spv::ImageFormat::R16ui;
                    case 0x8232: /*GL_R8UI*/ return spv::ImageFormat::R8ui;
                    case 0x8D82: /*GL_RGBA32I*/ return spv::ImageFormat::Rgba32i;
                    case 0x8D88: /*GL_RGBA16I*/ return spv::ImageFormat::Rgba16i;
                    case 0x8D8E: /*GL_RGBA8I*/ return spv::ImageFormat::Rgba8i;
                    case 0x823B: /*GL_RG32I*/ return spv::ImageFormat::Rg32i;
                    case 0x8239: /*GL_RG16I*/ return spv::ImageFormat::Rg16i;
                    case 0x8237: /*GL_RG8I*/ return spv::ImageFormat::Rg8i;
                    case 0x8235: /*GL_R32I*/ return spv::ImageFormat::R32i;
                    case 0x8233: /*GL_R16I*/ return spv::ImageFormat::R16i;
                    case 0x8231: /*GL_R8I*/ return spv::ImageFormat::R8i;
                    case 0x8058: /*GL_RGBA8*/ return spv::ImageFormat::Rgba8;
                    case 0x805B: /*GL_RGBA16*/ return spv::ImageFormat::Rgba16;
                    case 0x8059: /*GL_RGB10_A2*/ return spv::ImageFormat::Rgb10A2;
                    case 0x822B: /*GL_RG8*/ return spv::ImageFormat::Rg8;
                    case 0x822C: /*GL_RG16*/ return spv::ImageFormat::Rg16;
                    case 0x8229: /*GL_R8*/ return spv::ImageFormat::R8;
                    case 0x822A: /*GL_R16*/ return spv::ImageFormat::R16;
                    case 0x8F97: /*GL_RGBA8_SNORM*/ return spv::ImageFormat::Rgba8Snorm;
                    case 0x8F9B: /*GL_RGBA16_SNORM*/ return spv::ImageFormat::Rgba16Snorm;
                    case 0x8F95: /*GL_RG8_SNORM*/ return spv::ImageFormat::Rg8Snorm;
                    case 0x8F99: /*GL_RG16_SNORM*/ return spv::ImageFormat::Rg16Snorm;
                    case 0x8F94: /*GL_R8_SNORM*/ return spv::ImageFormat::R8Snorm;
                    case 0x8F98: /*GL_R16_SNORM*/ return spv::ImageFormat::R16Snorm;
                    default:
                        return spv::ImageFormat::Unknown;
                    }
                }

                Uint GLInternalFormatOfSpirvImageFormat(spv::ImageFormat format) {
                    switch (format) {
                    case spv::ImageFormat::Rgba32f: return 0x8814; // GL_RGBA32F
                    case spv::ImageFormat::Rgba16f: return 0x881A; // GL_RGBA16F
                    case spv::ImageFormat::Rgba8: return 0x8058;   // GL_RGBA8
                    case spv::ImageFormat::Rgba8Snorm: return 0x8F97; // GL_RGBA8_SNORM
                    case spv::ImageFormat::Rgba32i: return 0x8D82; // GL_RGBA32I
                    case spv::ImageFormat::Rgba16i: return 0x8D88; // GL_RGBA16I
                    case spv::ImageFormat::Rgba8i: return 0x8D8E;  // GL_RGBA8I
                    case spv::ImageFormat::Rgba32ui: return 0x8D70; // GL_RGBA32UI
                    case spv::ImageFormat::Rgba16ui: return 0x8D76; // GL_RGBA16UI
                    case spv::ImageFormat::Rgba8ui: return 0x8D7C;  // GL_RGBA8UI
                    default:
                        // Only the carriers need the reverse direction, and every carrier is one
                        // of the four-channel core formats above.
                        return 0;
                    }
                }

                uint32_t ChannelsOfSpirvImageFormat(spv::ImageFormat format) {
                    switch (format) {
                    case spv::ImageFormat::R32f:
                    case spv::ImageFormat::R16f:
                    case spv::ImageFormat::R16:
                    case spv::ImageFormat::R8:
                    case spv::ImageFormat::R16Snorm:
                    case spv::ImageFormat::R8Snorm:
                    case spv::ImageFormat::R32i:
                    case spv::ImageFormat::R16i:
                    case spv::ImageFormat::R8i:
                    case spv::ImageFormat::R32ui:
                    case spv::ImageFormat::R16ui:
                    case spv::ImageFormat::R8ui:
                        return 1;
                    case spv::ImageFormat::Rg32f:
                    case spv::ImageFormat::Rg16f:
                    case spv::ImageFormat::Rg16:
                    case spv::ImageFormat::Rg8:
                    case spv::ImageFormat::Rg16Snorm:
                    case spv::ImageFormat::Rg8Snorm:
                    case spv::ImageFormat::Rg32i:
                    case spv::ImageFormat::Rg16i:
                    case spv::ImageFormat::Rg8i:
                    case spv::ImageFormat::Rg32ui:
                    case spv::ImageFormat::Rg16ui:
                    case spv::ImageFormat::Rg8ui:
                        return 2;
                    case spv::ImageFormat::R11fG11fB10f:
                        return 3;
                    case spv::ImageFormat::Rgba32f:
                    case spv::ImageFormat::Rgba16f:
                    case spv::ImageFormat::Rgba16:
                    case spv::ImageFormat::Rgb10A2:
                    case spv::ImageFormat::Rgba8:
                    case spv::ImageFormat::Rgba16Snorm:
                    case spv::ImageFormat::Rgba8Snorm:
                    case spv::ImageFormat::Rgba32i:
                    case spv::ImageFormat::Rgba16i:
                    case spv::ImageFormat::Rgba8i:
                    case spv::ImageFormat::Rgba32ui:
                    case spv::ImageFormat::Rgba16ui:
                    case spv::ImageFormat::Rgba8ui:
                    case spv::ImageFormat::Rgb10a2ui:
                        return 4;
                    default:
                        return 0;
                    }
                }

                Bool IsWidenableStorageImageType(const Instruction* type) {
                    if (type == nullptr || type->opcode() != spv::Op::OpTypeImage) return false;
                    if (type->GetSingleWordInOperand(kImageSampledOperand) != kSampledStorageImage) return false;
                    const auto format =
                        static_cast<spv::ImageFormat>(type->GetSingleWordInOperand(kImageFormatOperand));
                    return static_cast<Bool>(WideningOfSpirvImageFormat(format));
                }
            } // namespace

            Uint WidenImageFormatsPass::WidenedCoreEsslImageFormat(Uint glInternalFormat) {
                const ImageFormatWidening widening =
                    WideningOfSpirvImageFormat(SpirvImageFormatOfGL(glInternalFormat));
                if (!widening) return 0;
                return GLInternalFormatOfSpirvImageFormat(widening.Carrier);
            }

            Uint WidenImageFormatsPass::ImageFormatChannelCount(Uint glInternalFormat) {
                return ChannelsOfSpirvImageFormat(SpirvImageFormatOfGL(glInternalFormat));
            }

            bool WidenImageFormatsPass::DeclaresWidenableImageFormat(IRContext* context) {
                if (context == nullptr) {
                    return false;
                }
                for (const Instruction& type : context->module()->types_values()) {
                    if (IsWidenableStorageImageType(&type)) {
                        return true;
                    }
                }
                return false;
            }

            bool WidenImageFormatsPass::DeclaresWidenableImageFormat(const Vector<Uint32>& binary) {
                std::unique_ptr<IRContext> context = spvtools::BuildModule(
                    SPV_ENV_VULKAN_1_1, [](spv_message_level_t, const char*, const spv_position_t&, const char*) {},
                    binary.data(), binary.size());
                return DeclaresWidenableImageFormat(context.get());
            }

            spvtools::opt::Pass::Status WidenImageFormatsPass::Process() {
                auto* irContext = context();
                auto* defUseMgr = irContext->get_def_use_mgr();

                // Cheap gate first: no widenable image type, and the module is handed back
                // byte-identical - which is every shader but a handful.
                std::vector<Instruction*> imageTypes;
                for (Instruction& type : irContext->types_values()) {
                    if (IsWidenableStorageImageType(&type)) {
                        imageTypes.push_back(&type);
                    }
                }
                if (imageTypes.empty()) {
                    return Status::SuccessWithoutChange;
                }

                // What each widenable image type becomes, and the mask its accesses take. Keyed on
                // the type's result id so the access walk below can ask about an image VALUE by
                // its type without re-deriving anything.
                struct WidenedImage {
                    spv::ImageFormat Carrier = spv::ImageFormat::Unknown;
                    uint32_t Channels = 0;
                    uint32_t SampledTypeId = 0;
                };
                std::map<uint32_t, WidenedImage> widenedByTypeId;
                for (Instruction* type : imageTypes) {
                    const auto format =
                        static_cast<spv::ImageFormat>(type->GetSingleWordInOperand(kImageFormatOperand));
                    const ImageFormatWidening widening = WideningOfSpirvImageFormat(format);
                    widenedByTypeId.emplace(type->result_id(),
                                            WidenedImage{widening.Carrier, widening.Channels,
                                                         type->GetSingleWordInOperand(kImageSampledTypeOperand)});
                }

                // Collect the accesses BEFORE anything is mutated, and refuse the whole rewrite if
                // any of them is a shape this pass cannot mask end to end. A widened declaration
                // whose accesses were left unmasked is worse than the compile error it replaced:
                // the shader runs and quietly reads the carrier's surplus channels, which GL says
                // are 0 and 1. Refusing hands the stage back to the "no GLSL ES spelling"
                // diagnostic instead, which at least names the failure.
                std::vector<Instruction*> reads;
                std::vector<Instruction*> writes;
                Bool rewritable = true;
                for (auto funcIt = irContext->module()->begin();
                     funcIt != irContext->module()->end() && rewritable; ++funcIt) {
                    funcIt->ForEachInst([&](Instruction* inst) {
                        if (!rewritable) return;
                        switch (inst->opcode()) {
                        case spv::Op::OpImageRead:
                        case spv::Op::OpImageWrite:
                        case spv::Op::OpImageSparseRead:
                        case spv::Op::OpImageTexelPointer:
                            break;
                        default:
                            return;
                        }
                        // OpImageTexelPointer names the image VARIABLE (a pointer), the other
                        // three an image VALUE; both reach the OpTypeImage through the def's
                        // type, one hop further for the pointer.
                        const Instruction* imageDef =
                            defUseMgr->GetDef(inst->GetSingleWordInOperand(kImageAccessImageOperand));
                        if (imageDef == nullptr) return;
                        uint32_t imageTypeId = imageDef->type_id();
                        if (const Instruction* imageType = defUseMgr->GetDef(imageTypeId);
                            imageType != nullptr && imageType->opcode() == spv::Op::OpTypePointer) {
                            imageTypeId = imageType->GetSingleWordInOperand(1);
                        }
                        const auto widenedIt = widenedByTypeId.find(imageTypeId);
                        if (widenedIt == widenedByTypeId.end()) return;

                        if (inst->opcode() == spv::Op::OpImageRead) {
                            reads.push_back(inst);
                            return;
                        }
                        if (inst->opcode() == spv::Op::OpImageWrite) {
                            writes.push_back(inst);
                            return;
                        }
                        // OpImageSparseRead yields a struct rather than a plain texel vector, and
                        // OpImageTexelPointer is an image atomic - which spirv-val already
                        // restricts to r32i/r32ui/r32f, all three of them core formats that never
                        // reach this table. Neither is expressible in the ESSL this backend emits,
                        // so rather than mask a shape that has never been seen, decline.
                        rewritable = false;
                    });
                }
                if (!rewritable) {
                    return Status::SuccessWithoutChange;
                }

                // The four-component (0, .., 0, 1) constant each mask shuffles its surplus
                // channels out of, one per component type in play. GL defines an imageLoad from a
                // format with fewer than four channels as (r, 0, 0, 1) and an imageStore as
                // dropping the components the format does not have, so pinning the carrier's
                // surplus channels to exactly these values is the whole of the emulation.
                std::map<uint32_t, uint32_t> zeroOneConstantBySampledType; // sampled type id -> constant id
                std::map<uint32_t, uint32_t> vec4TypeBySampledType;        // sampled type id -> v4 type id
                auto resolveMaskMaterial = [&](uint32_t sampledTypeId, uint32_t& outConstantId,
                                               uint32_t& outVec4TypeId) -> Bool {
                    if (const auto cached = zeroOneConstantBySampledType.find(sampledTypeId);
                        cached != zeroOneConstantBySampledType.end()) {
                        outConstantId = cached->second;
                        outVec4TypeId = vec4TypeBySampledType[sampledTypeId];
                        return outConstantId != 0 && outVec4TypeId != 0;
                    }
                    const Instruction* sampledType = defUseMgr->GetDef(sampledTypeId);
                    if (sampledType == nullptr) return false;

                    uint32_t oneWord = 0;
                    std::unique_ptr<analysis::Type> component;
                    if (sampledType->opcode() == spv::Op::OpTypeFloat &&
                        sampledType->GetSingleWordInOperand(0) == 32) {
                        component = spvtools::MakeUnique<analysis::Float>(32);
                        oneWord = 0x3F800000u; // 1.0f
                    } else if (sampledType->opcode() == spv::Op::OpTypeInt &&
                               sampledType->GetSingleWordInOperand(0) == 32) {
                        // OpTypeInt in-operands: 0 width, 1 signedness.
                        component = spvtools::MakeUnique<analysis::Integer>(
                            32, sampledType->GetSingleWordInOperand(1) != 0);
                        oneWord = 1u;
                    } else {
                        return false;
                    }

                    auto* typeMgr = irContext->get_type_mgr();
                    auto* constantMgr = irContext->get_constant_mgr();
                    analysis::Type* componentReg = typeMgr->GetRegisteredType(component.get());
                    if (componentReg == nullptr) return false;
                    const analysis::Constant* zero = constantMgr->GetConstant(componentReg, {0u});
                    const analysis::Constant* one = constantMgr->GetConstant(componentReg, {oneWord});
                    if (zero == nullptr || one == nullptr) return false;
                    const Instruction* zeroInst = constantMgr->GetDefiningInstruction(zero);
                    const Instruction* oneInst = constantMgr->GetDefiningInstruction(one);
                    if (zeroInst == nullptr || oneInst == nullptr) return false;

                    analysis::Vector vector(componentReg, 4);
                    const uint32_t vec4TypeId = typeMgr->GetTypeInstruction(&vector);
                    if (vec4TypeId == 0) return false;
                    // Through the id rather than through GetRegisteredType(&vector): the
                    // instruction the line above declared (or found) is the one the constant has
                    // to be typed by, and asking the manager for its type is what guarantees the
                    // two are the same registered object.
                    analysis::Type* vectorReg = typeMgr->GetType(vec4TypeId);
                    if (vectorReg == nullptr) return false;
                    // A vector constant's "literal words" are the IDS of its components
                    // (ConstantManager::CreateConstant -> GetConstantsFromIds).
                    const analysis::Constant* zeroOne = constantMgr->GetConstant(
                        vectorReg, {zeroInst->result_id(), zeroInst->result_id(), zeroInst->result_id(),
                                    oneInst->result_id()});
                    if (zeroOne == nullptr) return false;
                    const Instruction* zeroOneInst = constantMgr->GetDefiningInstruction(zeroOne);
                    if (zeroOneInst == nullptr) return false;

                    outConstantId = zeroOneInst->result_id();
                    outVec4TypeId = vec4TypeId;
                    zeroOneConstantBySampledType.emplace(sampledTypeId, outConstantId);
                    vec4TypeBySampledType.emplace(sampledTypeId, outVec4TypeId);
                    return true;
                };

                // OpVectorShuffle selects components 0-3 from the first vector and 4-7 from the
                // second, so with (0, 0, 0, 1) as the second operand the mask for a `channels`-
                // channel format is [0 .. channels-1] followed by 4 + i for the rest: the surplus
                // channels take the constant's 0s and, at index 3, its 1.
                auto maskComponents = [](uint32_t channels) {
                    std::vector<Operand> components;
                    components.reserve(4);
                    for (uint32_t i = 0; i < 4; ++i) {
                        components.push_back(
                            {SPV_OPERAND_TYPE_LITERAL_INTEGER, {i < channels ? i : 4u + i}});
                    }
                    return components;
                };

                auto widenedOf = [&](const Instruction* inst) -> const WidenedImage* {
                    const Instruction* imageDef =
                        defUseMgr->GetDef(inst->GetSingleWordInOperand(kImageAccessImageOperand));
                    if (imageDef == nullptr) return nullptr;
                    const auto it = widenedByTypeId.find(imageDef->type_id());
                    return it == widenedByTypeId.end() ? nullptr : &it->second;
                };

                // Every constant and vector type the masks will need, declared BEFORE the first
                // instruction is inserted. The constant and type managers append to the module's
                // globals and keep their own def-use bookkeeping straight; the shuffles below do
                // not (this pass invalidates every analysis at the end instead), so doing the two
                // in the other order would have the managers consult a def-use map that no longer
                // describes the function bodies.
                for (const auto& widened : widenedByTypeId) {
                    uint32_t unusedConstantId = 0;
                    uint32_t unusedVec4TypeId = 0;
                    if (!resolveMaskMaterial(widened.second.SampledTypeId, unusedConstantId, unusedVec4TypeId)) {
                        return Status::SuccessWithoutChange;
                    }
                }

                // Masks first, while every image type still carries its ORIGINAL format: the
                // rewrite below only touches the format operand, so the accesses' types do not
                // move and the order is free either way - but doing it first keeps a failed
                // resolve from leaving a half-widened module behind.
                for (Instruction* write : writes) {
                    const WidenedImage* widened = widenedOf(write);
                    if (widened == nullptr) continue;
                    uint32_t zeroOneId = 0;
                    uint32_t vec4TypeId = 0;
                    if (!resolveMaskMaterial(widened->SampledTypeId, zeroOneId, vec4TypeId)) {
                        return Status::SuccessWithoutChange;
                    }
                    const uint32_t texelId = write->GetSingleWordInOperand(kImageWriteTexelOperand);
                    const Instruction* texel = defUseMgr->GetDef(texelId);
                    // SPIR-V allows a scalar texel; GLSL's imageStore always passes a gvec4, and a
                    // shape this has never seen is refused rather than guessed at.
                    if (texel == nullptr || texel->type_id() != vec4TypeId) {
                        return Status::SuccessWithoutChange;
                    }
                    const uint32_t maskedId = irContext->TakeNextId();
                    if (maskedId == 0) return Status::Failure;
                    Instruction::OperandList shuffleOperands{{SPV_OPERAND_TYPE_ID, {texelId}},
                                                             {SPV_OPERAND_TYPE_ID, {zeroOneId}}};
                    for (const Operand& component : maskComponents(widened->Channels)) {
                        shuffleOperands.push_back(component);
                    }
                    write->InsertBefore(spvtools::MakeUnique<Instruction>(
                        irContext, spv::Op::OpVectorShuffle, vec4TypeId, maskedId, shuffleOperands));
                    write->SetInOperand(kImageWriteTexelOperand, {maskedId});
                }

                for (Instruction* read : reads) {
                    const WidenedImage* widened = widenedOf(read);
                    if (widened == nullptr) continue;
                    uint32_t zeroOneId = 0;
                    uint32_t vec4TypeId = 0;
                    if (!resolveMaskMaterial(widened->SampledTypeId, zeroOneId, vec4TypeId)) {
                        return Status::SuccessWithoutChange;
                    }
                    if (read->type_id() != vec4TypeId) {
                        return Status::SuccessWithoutChange;
                    }
                    // The ORIGINAL instruction keeps its result id and becomes the shuffle, and a
                    // copy of the read is inserted in front of it under a fresh id. That way every
                    // existing use of the read stays intact without a ReplaceAllUsesWith that
                    // would also rewrite the shuffle's own operand (the idiom
                    // EmulateNoPerspectivePass uses for the same reason).
                    const uint32_t rawReadId = irContext->TakeNextId();
                    if (rawReadId == 0) return Status::Failure;
                    Instruction::OperandList readOperands;
                    for (uint32_t i = 0; i < read->NumInOperands(); ++i) {
                        readOperands.push_back(read->GetInOperand(i));
                    }
                    read->InsertBefore(spvtools::MakeUnique<Instruction>(
                        irContext, spv::Op::OpImageRead, vec4TypeId, rawReadId, readOperands));
                    read->SetOpcode(spv::Op::OpVectorShuffle);
                    Instruction::OperandList shuffleOperands{{SPV_OPERAND_TYPE_ID, {rawReadId}},
                                                             {SPV_OPERAND_TYPE_ID, {zeroOneId}}};
                    for (const Operand& component : maskComponents(widened->Channels)) {
                        shuffleOperands.push_back(component);
                    }
                    read->SetInOperands(Move(shuffleOperands));
                }

                // The declaration itself, last. Only the format operand moves: the carrier has the
                // same component type as the original by construction, so the OpTypeImage's
                // Sampled Type still agrees with it (which is what spirv-val checks) and no
                // pointer, array or access-chain type has to be rebuilt.
                //
                // Two image types can COLLIDE here - `layout(rg32f)` and `layout(rgba32f)` in one
                // module both become Rgba32f - and duplicate non-aggregate type declarations are
                // invalid SPIR-V. The caller runs spirv-tools' RemoveDuplicates pass immediately
                // after this one, which joins them (and cascades to the pointer and array types
                // that named them) rather than this pass carrying its own join.
                for (Instruction* type : imageTypes) {
                    const auto widenedIt = widenedByTypeId.find(type->result_id());
                    if (widenedIt == widenedByTypeId.end()) continue;
                    // No def-use re-analysis: the Image Format operand is a LITERAL, so no use of
                    // any id moves, and the masks above already left the manager describing a
                    // module that has since grown instructions it was never told about. Every
                    // analysis is dropped below instead.
                    type->SetInOperand(kImageFormatOperand, {static_cast<uint32_t>(widenedIt->second.Carrier)});
                }

                // StorageImageExtendedFormats is deliberately left declared even though every
                // remaining format is now one of the thirteen that need no capability: a
                // capability a module no longer exercises is valid SPIR-V, and dropping one is
                // only safe after proving no extended format is left ANYWHERE, including in image
                // types this pass declined.
                irContext->InvalidateAnalysesExceptFor(IRContext::kAnalysisNone);
                return Status::SuccessWithChange;
            }

            spvtools::Optimizer::PassToken WidenImageFormatsPass::CreateWidenImageFormatsPass() {
                return spvtools::Optimizer::PassToken(spvtools::MakeUnique<WidenImageFormatsPass>());
            }
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
