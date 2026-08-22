// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/WidenImageFormatsPass.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "WidenImageFormatsPass.h"

// For IsSpirvCrossEsslPrintableFormat: the two passes share one question about the emitter, and
// the answer belongs where the rest of the image-format tables already are.
#include "BakeImageFormatsPass.h"

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
#include "source/util/string_utils.h"

#include <cstring>
#include <map>
#include <memory>
#include <set>
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
                constexpr uint32_t kImageDimOperand = 1;
                constexpr uint32_t kImageSampledOperand = 5;
                constexpr uint32_t kImageFormatOperand = 6;
                // A storage image, i.e. one reached through imageLoad/imageStore rather than a
                // sampler. The only kind that carries a format qualifier in any GLSL dialect.
                constexpr uint32_t kSampledStorageImage = 2;

                // OpImageRead in-operands: 0 image, 1 coordinate, 2.. optional image operands.
                // OpImageWrite in-operands: 0 image, 1 coordinate, 2 texel, 3.. optional.
                constexpr uint32_t kImageAccessImageOperand = 0;
                constexpr uint32_t kImageWriteTexelOperand = 2;

                // The carrier of a non-core image format: a core GLSL ES format that represents
                // every value the original can hold, WITHOUT LOSS. `channels` is what the original
                // format really has, which is what every access through the carrier is masked back
                // to.
                //
                // Almost every entry is a pure CHANNEL widening - same component type, same
                // per-channel width, more channels (rg32f -> rgba32f) - and for those the carrier
                // is bit-exact: the storage holds the identical encoding, only wider.
                //
                // Two entries are not. r11f_g11f_b10f has no same-width core carrier, so it takes
                // rgba16f, and the two encodings differ. What matters is that the carrier is still
                // LOSSLESS: an 11-bit float is e5m6 and a 10-bit float is e5m5, while a half is
                // s1e5m10 - the SAME 5-bit exponent with a strictly longer mantissa - so every
                // value the packed format can represent has an exact half. Nothing an application
                // stores is rounded away.
                //
                // rgb10_a2ui is the other. Its four channels are 10, 10, 10 and 2 bits of UNSIGNED
                // INTEGER, and rgba16ui gives each of them sixteen - every value of every channel
                // fits, with the same component type and the same channel COUNT, so nothing is
                // masked and nothing is re-encoded on the shader side at all. Only the transfer
                // differs: the frontend's shadow for it is one packed 32-bit word per texel
                // (GL_UNSIGNED_INT_2_10_10_10_REV), so the upload has to split that word into four
                // shorts the way r11f_g11f_b10f's has to be decoded into four floats.
                //
                // What DOES change is the reverse direction: the carrier can hold values the
                // packed format could not - negatives (11f and 10f are unsigned), and mantissa
                // bits finer than the 6 and 5 the format quantises to - so a value written through
                // the image and then SAMPLED comes back on half's grid rather than the packed
                // format's. That is a strictly finer grid, never a lossy one, and it is measured
                // against the alternative, which is not a more faithful quantisation but no
                // program at all: `layout(r11f_g11f_b10f)` has no ESSL spelling, SPIRV-Cross
                // throws for it, and the stage - with every other image uniform declared beside it
                // - is lost (KHR-GL43.shader_image_load_store.basic-allFormats-*, which fail on
                // this format alone, and multiple-uniforms, where one such declaration killed a
                // program holding eight images).
                //
                // The remaining seven - rgb10_a2, rgba16, rg16, r16, rgba16_snorm, rg16_snorm and
                // r16_snorm - are NORMALIZED, and core ESSL has no 16-bit normalized format at all
                // and no 10-bit one. There is no carrier that keeps their component type, and no
                // FLOAT carrier that is honest either: a half has eleven mantissa bits against a
                // 16-bit normalized channel's sixteen, so rgba16f would quantise. What DOES hold
                // every one of their values exactly is the format's own CODE: a normalized channel
                // of b bits is an integer in [0, 2^b-1] (unsigned) or [-(2^(b-1)-1), 2^(b-1)-1]
                // (signed), and rgba16ui gives every channel of all seven sixteen bits to hold
                // that integer in - bit for bit, with the SAME quantisation grid the real format
                // has, which is the one thing a float carrier could not reproduce.
                //
                // The price is that the carrier changes the SHADER-VISIBLE TYPE: an image2D
                // becomes a uimage2D, so every imageLoad has to divide the code back out and every
                // imageStore has to round a value onto it (GL 4.6 2.3.5). ChannelMax below is the
                // denominator that conversion uses, per channel - the same number for all four of
                // a 16-bit format and (1023, 1023, 1023, 3) for rgb10_a2, whose channels are not
                // all the same width.
                //
                // What this carrier gives up, and it is real: the ES texture behind the image is
                // now an INTEGER texture, so a `sampler2D` bound to it reads codes rather than the
                // normalized value, and it can no longer be filtered. Measured against the
                // alternative, which is not a truer sampler but no program at all - the stage that
                // declares one of these seven has no legal ESSL, so before this it did not compile
                // and nothing sampled anything.
                struct ImageFormatWidening {
                    spv::ImageFormat Carrier = spv::ImageFormat::Unknown;
                    uint32_t Channels = 0;
                    // Non-zero when the carrier holds the format's channels as the INTEGER CODES
                    // of a NORMALIZED value rather than as the values themselves: the largest code
                    // each channel can hold, i.e. 2^b - 1 for an unsigned normalized channel of b
                    // bits and 2^(b-1) - 1 for a signed one.
                    uint32_t ChannelMax[4] = {0u, 0u, 0u, 0u};
                    bool SignedNormalized = false;

                    bool CarriesNormalizedCodes() const { return ChannelMax[0] != 0u; }
                    explicit operator bool() const { return Carrier != spv::ImageFormat::Unknown; }
                };

                constexpr uint32_t kUnorm16Max = 65535u;
                constexpr uint32_t kSnorm16Max = 32767u;

                ImageFormatWidening WideningOfSpirvImageFormat(spv::ImageFormat format) {
                    switch (format) {
                    // Float.
                    case spv::ImageFormat::Rg32f: return {spv::ImageFormat::Rgba32f, 2};
                    case spv::ImageFormat::Rg16f: return {spv::ImageFormat::Rgba16f, 2};
                    case spv::ImageFormat::R16f: return {spv::ImageFormat::Rgba16f, 1};
                    // Not a channel widening but a lossless re-encoding - see above. Three
                    // channels, so the fourth reads as the 1 GL defines for a format without one.
                    case spv::ImageFormat::R11fG11fB10f: return {spv::ImageFormat::Rgba16f, 3};
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
                    // FOUR channels, so there is no surplus channel to mask and no access is
                    // rewritten - 10, 10, 10 and 2 bits of unsigned integer all fit in sixteen.
                    case spv::ImageFormat::Rgb10a2ui: return {spv::ImageFormat::Rgba16ui, 4};
                    // Unsigned normalized, carried as codes in [0, 2^b - 1].
                    case spv::ImageFormat::Rgba16:
                        return {spv::ImageFormat::Rgba16ui, 4,
                                {kUnorm16Max, kUnorm16Max, kUnorm16Max, kUnorm16Max}, false};
                    case spv::ImageFormat::Rg16:
                        return {spv::ImageFormat::Rgba16ui, 2,
                                {kUnorm16Max, kUnorm16Max, kUnorm16Max, kUnorm16Max}, false};
                    case spv::ImageFormat::R16:
                        return {spv::ImageFormat::Rgba16ui, 1,
                                {kUnorm16Max, kUnorm16Max, kUnorm16Max, kUnorm16Max}, false};
                    // The one entry whose channels are not all the same width, which is the whole
                    // reason ChannelMax is per channel rather than one number.
                    case spv::ImageFormat::Rgb10A2:
                        return {spv::ImageFormat::Rgba16ui, 4, {1023u, 1023u, 1023u, 3u}, false};
                    // Signed normalized, carried as the two's-complement code in [-(2^(b-1) - 1),
                    // 2^(b-1) - 1]. The carrier's channel is UNSIGNED, so the code's sixteen bits
                    // are stored verbatim and sign-extended again on the way out.
                    case spv::ImageFormat::Rgba16Snorm:
                        return {spv::ImageFormat::Rgba16ui, 4,
                                {kSnorm16Max, kSnorm16Max, kSnorm16Max, kSnorm16Max}, true};
                    case spv::ImageFormat::Rg16Snorm:
                        return {spv::ImageFormat::Rgba16ui, 2,
                                {kSnorm16Max, kSnorm16Max, kSnorm16Max, kSnorm16Max}, true};
                    case spv::ImageFormat::R16Snorm:
                        return {spv::ImageFormat::Rgba16ui, 1,
                                {kSnorm16Max, kSnorm16Max, kSnorm16Max, kSnorm16Max}, true};
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

                Bool IsWidenableStorageImageType(const Instruction* type,
                                                 bool onlyFormatsSpirvCrossRefusesToPrint) {
                    if (type == nullptr || type->opcode() != spv::Op::OpTypeImage) return false;
                    if (type->GetSingleWordInOperand(kImageSampledOperand) != kSampledStorageImage) return false;
                    // A BUFFER image is never widened, whatever its format. Widening works because
                    // the ES texture behind the image can be REALLOCATED in the carrier, so the
                    // texel the shader addresses and the texel the storage holds stay the same
                    // size. A buffer image has no storage of its own to reallocate: its texels are
                    // the application's buffer object, at the size and layout the application gave
                    // it, and that buffer is usually also a vertex, index or storage buffer whose
                    // contents are not ours to relayout.
                    //
                    // Widening one anyway makes the shader stride 16 bytes through 8-byte texels.
                    // Measured on an Adreno 830 with a 32-byte GL_RG32F buffer and a shader storing
                    // (i+1, 100) at texel i: the readback came back [1,100] [0,1] [2,100] [0,1] -
                    // texels 0 and 1 landed on top of all four, texels 2 and 3 ran off the end of
                    // the application's buffer. Declining leaves the honest "no GLSL ES spelling"
                    // failure instead, which loses the same stage but corrupts nothing.
                    if (static_cast<spv::Dim>(type->GetSingleWordInOperand(kImageDimOperand)) ==
                        spv::Dim::Buffer) {
                        return false;
                    }
                    const auto format =
                        static_cast<spv::ImageFormat>(type->GetSingleWordInOperand(kImageFormatOperand));
                    if (!WideningOfSpirvImageFormat(format)) return false;
                    if (onlyFormatsSpirvCrossRefusesToPrint &&
                        BakeImageFormatsPass::IsSpirvCrossEsslPrintableFormat(static_cast<Uint32>(format))) {
                        // The driver can spell this one and the emitter will print it; widening it
                        // would spend two to four times the texture memory to change nothing.
                        return false;
                    }
                    return true;
                }

                // GLSL.std.450 instruction numbers (see 3rdparty/glslang/SPIRV/GLSL.std.450.h).
                constexpr uint32_t kGlslFSign = 6u;
                constexpr uint32_t kGlslFMax = 40u;
                constexpr uint32_t kGlslFClamp = 43u;

                // The module's GLSL.std.450 import, creating it when the module has none. glslang
                // emits one for all but the most trivial shaders, but a module that reached here
                // without one still has to be carriable. 0 means no id was available and NOTHING
                // was added, so the caller can still hand the module back untouched.
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

                // Every type and constant the normalized-code rewrite emits, resolved ONCE before
                // any instruction is inserted. The type and constant managers append to the
                // module's globals and keep their own def-use bookkeeping straight; the rewrite
                // below does not (this pass drops every analysis at the end instead), so a manager
                // consulted after the first insertion would be reading a def-use map that no
                // longer describes the function bodies.
                struct NormalizedCarrierMaterial {
                    uint32_t Glsl450Id = 0;
                    uint32_t FloatTypeId = 0;  // the component types, kept only so the declaration
                    uint32_t IntTypeId = 0;    // order below can put each vector after its own
                    uint32_t UintTypeId = 0;   // component - and the last is the image's new Sampled Type
                    uint32_t UvecTypeId = 0;   // uvec4: what an OpImageRead of the carrier yields
                    uint32_t IvecTypeId = 0;   // ivec4: the sign-extended snorm code
                    uint32_t FvecTypeId = 0;   // vec4: what the shader asked for
                    uint32_t ShiftWidthId = 0; // ivec4(16), the snorm sign extension
                    uint32_t LowWordMaskId = 0; // uvec4(0xFFFF)
                    uint32_t ZeroId = 0;       // vec4(0.0)
                    uint32_t OneId = 0;        // vec4(1.0)
                    uint32_t MinusOneId = 0;   // vec4(-1.0)
                    uint32_t HalfId = 0;       // vec4(0.5)

                    explicit operator bool() const { return UvecTypeId != 0u; }
                };

                // A four-component constant of `typeId` from four component ids.
                uint32_t MakeVec4Constant(IRContext* context, uint32_t typeId, const uint32_t (&componentIds)[4]) {
                    auto* constantMgr = context->get_constant_mgr();
                    analysis::Type* vectorType = context->get_type_mgr()->GetType(typeId);
                    if (vectorType == nullptr) return 0u;
                    // A vector constant's "literal words" are the IDS of its components
                    // (ConstantManager::CreateConstant -> GetConstantsFromIds).
                    const analysis::Constant* constant = constantMgr->GetConstant(
                        vectorType, {componentIds[0], componentIds[1], componentIds[2], componentIds[3]});
                    if (constant == nullptr) return 0u;
                    const Instruction* definition = constantMgr->GetDefiningInstruction(constant);
                    return definition == nullptr ? 0u : definition->result_id();
                }

                uint32_t MakeScalarConstant(IRContext* context, analysis::Type* scalarType, uint32_t word) {
                    const analysis::Constant* constant = context->get_constant_mgr()->GetConstant(scalarType, {word});
                    if (constant == nullptr) return 0u;
                    const Instruction* definition =
                        context->get_constant_mgr()->GetDefiningInstruction(constant);
                    return definition == nullptr ? 0u : definition->result_id();
                }

                uint32_t MakeSplatVec4Constant(IRContext* context, uint32_t vectorTypeId,
                                               analysis::Type* scalarType, uint32_t word) {
                    const uint32_t scalarId = MakeScalarConstant(context, scalarType, word);
                    if (scalarId == 0u) return 0u;
                    const uint32_t componentIds[4] = {scalarId, scalarId, scalarId, scalarId};
                    return MakeVec4Constant(context, vectorTypeId, componentIds);
                }

                uint32_t FloatBits(float value) {
                    uint32_t bits = 0;
                    static_assert(sizeof(bits) == sizeof(value), "float is not 32 bits");
                    std::memcpy(&bits, &value, sizeof(bits));
                    return bits;
                }

                NormalizedCarrierMaterial ResolveNormalizedCarrierMaterial(IRContext* context) {
                    NormalizedCarrierMaterial material;
                    auto* typeMgr = context->get_type_mgr();

                    analysis::Integer uintScalar(32, false);
                    analysis::Integer intScalar(32, true);
                    analysis::Float floatScalar(32);
                    analysis::Type* uintReg = typeMgr->GetRegisteredType(&uintScalar);
                    analysis::Type* intReg = typeMgr->GetRegisteredType(&intScalar);
                    analysis::Type* floatReg = typeMgr->GetRegisteredType(&floatScalar);
                    if (uintReg == nullptr || intReg == nullptr || floatReg == nullptr) return {};

                    analysis::Vector uintVector(uintReg, 4);
                    analysis::Vector intVector(intReg, 4);
                    analysis::Vector floatVector(floatReg, 4);
                    const uint32_t uintTypeId = typeMgr->GetTypeInstruction(&uintScalar);
                    const uint32_t uvecTypeId = typeMgr->GetTypeInstruction(&uintVector);
                    const uint32_t ivecTypeId = typeMgr->GetTypeInstruction(&intVector);
                    const uint32_t fvecTypeId = typeMgr->GetTypeInstruction(&floatVector);
                    if (uintTypeId == 0u || uvecTypeId == 0u || ivecTypeId == 0u || fvecTypeId == 0u) return {};

                    const uint32_t glsl450Id = EnsureGlslStd450Import(context);
                    if (glsl450Id == 0u) return {};

                    material.Glsl450Id = glsl450Id;
                    material.FloatTypeId = typeMgr->GetTypeInstruction(&floatScalar);
                    material.IntTypeId = typeMgr->GetTypeInstruction(&intScalar);
                    material.UintTypeId = uintTypeId;
                    material.UvecTypeId = uvecTypeId;
                    material.IvecTypeId = ivecTypeId;
                    material.FvecTypeId = fvecTypeId;
                    material.ShiftWidthId = MakeSplatVec4Constant(context, ivecTypeId, intReg, 16u);
                    material.LowWordMaskId = MakeSplatVec4Constant(context, uvecTypeId, uintReg, 0xFFFFu);
                    material.ZeroId = MakeSplatVec4Constant(context, fvecTypeId, floatReg, FloatBits(0.0f));
                    material.OneId = MakeSplatVec4Constant(context, fvecTypeId, floatReg, FloatBits(1.0f));
                    material.MinusOneId = MakeSplatVec4Constant(context, fvecTypeId, floatReg, FloatBits(-1.0f));
                    material.HalfId = MakeSplatVec4Constant(context, fvecTypeId, floatReg, FloatBits(0.5f));
                    if (material.ShiftWidthId == 0u || material.LowWordMaskId == 0u || material.ZeroId == 0u ||
                        material.OneId == 0u || material.MinusOneId == 0u || material.HalfId == 0u) {
                        return {};
                    }
                    return material;
                }

                // spirv-tools' type manager APPENDS a new type declaration to the END of the
                // module's type section - which is fine for a type only function bodies name, and
                // NOT fine for the uint32 an OpTypeImage further up is about to take as its
                // Sampled Type. SPIR-V requires an id to be defined before it is used, and
                // spirv-tools' own RemoveDuplicates - which the caller runs immediately after this
                // pass - walks the section in order and dereferences each image type's sampled
                // type as it goes, so an out-of-order declaration is a null dereference inside the
                // type manager rather than a diagnostic.
                //
                // `typeIdsInDependencyOrder` must list a component type before any vector of it:
                // each move lands immediately in front of `target`, so the order they are
                // processed in is the order they end up in.
                void HoistTypeDeclarationsBefore(IRContext* context, Instruction* target,
                                                 const std::vector<uint32_t>& typeIdsInDependencyOrder) {
                    std::set<uint32_t> definedBeforeTarget;
                    for (Instruction& declaration : context->module()->types_values()) {
                        if (&declaration == target) break;
                        definedBeforeTarget.insert(declaration.result_id());
                    }
                    for (const uint32_t typeId : typeIdsInDependencyOrder) {
                        if (typeId == 0u || definedBeforeTarget.count(typeId) != 0u) continue;
                        Instruction* declaration = context->get_def_use_mgr()->GetDef(typeId);
                        if (declaration == nullptr || declaration == target) continue;
                        // IntrusiveNodeBase::InsertBefore MOVES the node it is called on - it
                        // unlinks it from wherever it is first - which is the opposite convention
                        // to Instruction::InsertBefore(unique_ptr), used everywhere else here.
                        declaration->InsertBefore(target);
                    }
                }

                // vec4(ChannelMax), the denominator of the format's own normalized conversion.
                uint32_t ResolveDenominatorConstant(IRContext* context, const NormalizedCarrierMaterial& material,
                                                    const uint32_t (&channelMax)[4]) {
                    analysis::Float floatScalar(32);
                    analysis::Type* floatReg = context->get_type_mgr()->GetRegisteredType(&floatScalar);
                    if (floatReg == nullptr) return 0u;
                    uint32_t componentIds[4] = {0u, 0u, 0u, 0u};
                    for (uint32_t i = 0; i < 4; ++i) {
                        componentIds[i] = MakeScalarConstant(context, floatReg,
                                                             FloatBits(static_cast<float>(channelMax[i])));
                        if (componentIds[i] == 0u) return 0u;
                    }
                    return MakeVec4Constant(context, material.FvecTypeId, componentIds);
                }
            } // namespace

            Uint WidenImageFormatsPass::WidenedCoreEsslImageFormat(Uint glInternalFormat) {
                const ImageFormatWidening widening =
                    WideningOfSpirvImageFormat(SpirvImageFormatOfGL(glInternalFormat));
                if (!widening) return 0;
                return GLInternalFormatOfSpirvImageFormat(widening.Carrier);
            }

            bool WidenImageFormatsPass::NormalizedImageCarrierCodes(Uint glInternalFormat,
                                                                    Uint32 (&outChannelMax)[4],
                                                                    bool& outSignedNormalized) {
                const ImageFormatWidening widening =
                    WideningOfSpirvImageFormat(SpirvImageFormatOfGL(glInternalFormat));
                if (!widening || !widening.CarriesNormalizedCodes()) return false;
                for (Uint i = 0; i < 4; ++i) outChannelMax[i] = widening.ChannelMax[i];
                outSignedNormalized = widening.SignedNormalized;
                return true;
            }

            Uint WidenImageFormatsPass::ImageFormatChannelCount(Uint glInternalFormat) {
                return ChannelsOfSpirvImageFormat(SpirvImageFormatOfGL(glInternalFormat));
            }

            bool WidenImageFormatsPass::DeclaresWidenableImageFormat(
                IRContext* context, const bool onlyFormatsSpirvCrossRefusesToPrint) {
                if (context == nullptr) {
                    return false;
                }
                for (const Instruction& type : context->module()->types_values()) {
                    if (IsWidenableStorageImageType(&type, onlyFormatsSpirvCrossRefusesToPrint)) {
                        return true;
                    }
                }
                return false;
            }

            bool WidenImageFormatsPass::DeclaresWidenableImageFormat(
                const Vector<Uint32>& binary, const bool onlyFormatsSpirvCrossRefusesToPrint) {
                std::unique_ptr<IRContext> context = spvtools::BuildModule(
                    SPV_ENV_VULKAN_1_1, [](spv_message_level_t, const char*, const spv_position_t&, const char*) {},
                    binary.data(), binary.size());
                return DeclaresWidenableImageFormat(context.get(), onlyFormatsSpirvCrossRefusesToPrint);
            }

            spvtools::opt::Pass::Status WidenImageFormatsPass::Process() {
                auto* irContext = context();
                auto* defUseMgr = irContext->get_def_use_mgr();

                // Cheap gate first: no widenable image type, and the module is handed back
                // byte-identical - which is every shader but a handful.
                std::vector<Instruction*> imageTypes;
                for (Instruction& type : irContext->types_values()) {
                    if (IsWidenableStorageImageType(&type, m_onlyFormatsSpirvCrossRefusesToPrint)) {
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
                    // 0 unless the carrier holds NORMALIZED CODES, in which case it is the vec4 of
                    // per-channel denominators the conversion divides by and multiplies back up.
                    uint32_t DenominatorId = 0;
                    bool SignedNormalized = false;
                };
                std::map<uint32_t, WidenedImage> widenedByTypeId;
                Bool anyNormalizedCarrier = false;
                for (Instruction* type : imageTypes) {
                    const auto format =
                        static_cast<spv::ImageFormat>(type->GetSingleWordInOperand(kImageFormatOperand));
                    const ImageFormatWidening widening = WideningOfSpirvImageFormat(format);
                    anyNormalizedCarrier = anyNormalizedCarrier || widening.CarriesNormalizedCodes();
                    widenedByTypeId.emplace(
                        type->result_id(),
                        WidenedImage{widening.Carrier, widening.Channels,
                                     type->GetSingleWordInOperand(kImageSampledTypeOperand), 0u,
                                     widening.SignedNormalized});
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

                // ...and the same for the normalized carriers, whose rewrite needs a good deal
                // more of both: the uvec4 an OpImageRead of the carrier yields, the ivec4 the
                // signed code is sign-extended in, the GLSL.std.450 import the clamp and the sign
                // come from, and one vec4 of denominators per DISTINCT channel-width set (all
                // 65535 for the unsigned 16-bit formats, all 32767 for the signed ones, and
                // (1023, 1023, 1023, 3) for rgb10_a2, whose channels are not all the same width).
                NormalizedCarrierMaterial normalizedMaterial;
                if (anyNormalizedCarrier) {
                    normalizedMaterial = ResolveNormalizedCarrierMaterial(irContext);
                    if (!normalizedMaterial) {
                        return Status::SuccessWithoutChange;
                    }
                    Instruction* firstNormalizedImageType = nullptr;
                    for (Instruction* type : imageTypes) {
                        const auto widenedIt = widenedByTypeId.find(type->result_id());
                        if (widenedIt == widenedByTypeId.end()) continue;
                        const ImageFormatWidening widening = WideningOfSpirvImageFormat(
                            static_cast<spv::ImageFormat>(type->GetSingleWordInOperand(kImageFormatOperand)));
                        if (!widening.CarriesNormalizedCodes()) continue;
                        // The shader asked for a gvec4 of the ORIGINAL sampled type, and for these
                        // seven that type is float. An integer image declared with a normalized
                        // format is not something glslang can produce, so a module that somehow
                        // holds one is refused rather than converted through a type it never had.
                        const Instruction* sampledType = defUseMgr->GetDef(widenedIt->second.SampledTypeId);
                        if (sampledType == nullptr || sampledType->opcode() != spv::Op::OpTypeFloat ||
                            sampledType->GetSingleWordInOperand(0) != 32) {
                            return Status::SuccessWithoutChange;
                        }
                        const uint32_t denominatorId =
                            ResolveDenominatorConstant(irContext, normalizedMaterial, widening.ChannelMax);
                        if (denominatorId == 0u) {
                            return Status::SuccessWithoutChange;
                        }
                        widenedIt->second.DenominatorId = denominatorId;
                        if (firstNormalizedImageType == nullptr) firstNormalizedImageType = type;
                    }
                    // ...and the declarations have to reach the module in the right ORDER, not
                    // just exist. See HoistTypeDeclarationsBefore: the type manager appends, and
                    // the image type that names the new uint32 is already further up.
                    if (firstNormalizedImageType != nullptr) {
                        HoistTypeDeclarationsBefore(
                            irContext, firstNormalizedImageType,
                            {normalizedMaterial.FloatTypeId, normalizedMaterial.IntTypeId,
                             normalizedMaterial.UintTypeId, normalizedMaterial.FvecTypeId,
                             normalizedMaterial.IvecTypeId, normalizedMaterial.UvecTypeId});
                    }
                }

                // The two halves of GL 4.6 2.3.5 for a normalized carrier, spelled as SPIR-V.
                //
                //   UNPACK (imageLoad), unsigned: f = c / (2^b - 1)
                //   UNPACK (imageLoad), signed:   f = max(c / (2^(b-1) - 1), -1)
                //   PACK   (imageStore), unsigned: c = round(clamp(f, 0, 1) * (2^b - 1))
                //   PACK   (imageStore), signed:   c = round(clamp(f, -1, 1) * (2^(b-1) - 1))
                //
                // `round` is round-to-NEAREST, and GL leaves the tie direction to the
                // implementation ("if two values are equally near, the implementation may choose
                // either"). This one always rounds a tie AWAY FROM ZERO, which is a legal choice
                // and, unlike GLSL's own round(), a deterministic one - so the boundary cases can
                // be pinned by a test rather than described. It is spelled as a truncation of
                // x + 0.5*sign(x), because OpConvertFToU/OpConvertFToS truncate toward zero.
                //
                // A signed code is stored in an UNSIGNED carrier channel, so pack masks it to the
                // low sixteen bits (a negative uint32 is out of an rgba16ui channel's range, and
                // what a store does with an out-of-range integer is not defined) and unpack
                // sign-extends it back with a shift pair.
                //
                // Both operate on all FOUR channels at once, including the surplus ones a one- or
                // two-channel format does not have: the mask shuffle runs on the float side either
                // way, so whatever the surplus channels hold is discarded on the way out and
                // written as the format's own 0 and 1 on the way in.
                auto insertUnpack = [&](Instruction* before, const WidenedImage& widened,
                                        uint32_t rawId) -> uint32_t {
                    const auto emit = [&](spv::Op opcode, uint32_t typeId,
                                          Instruction::OperandList operands) -> uint32_t {
                        const uint32_t resultId = irContext->TakeNextId();
                        if (resultId == 0u) return 0u;
                        before->InsertBefore(spvtools::MakeUnique<Instruction>(irContext, opcode, typeId, resultId,
                                                                               Move(operands)));
                        return resultId;
                    };
                    // The raw texel through an OpCopyObject before anything reads it, which costs
                    // nothing in SPIR-V and is load-bearing in the ESSL: SPIRV-Cross emits a copy
                    // of a non-opaque value as a real `uvec4 _n = imageLoad(...);` statement,
                    // where the read's own result is FORWARDED into whatever consumes it. Mesa's
                    // llvmpipe compiler miscompiles the forwarded form - `vec4(imageLoad(img, c))`
                    // written straight into an expression comes back as zeroes, and 0xFFFF comes
                    // back as a NaN, while the identical arithmetic on a named uvec4 is correct.
                    // Measured with hand-written core-format ESSL (an rgba16ui uimage2D read into
                    // an rgba32f image2D), so it is the driver rather than anything this pass or
                    // the emitter does; the copy is the cheapest way to stay out of it, and every
                    // other driver folds it away.
                    const uint32_t texelId =
                        emit(spv::Op::OpCopyObject, normalizedMaterial.UvecTypeId,
                             {{SPV_OPERAND_TYPE_ID, {rawId}}});
                    if (texelId == 0u) return 0u;
                    uint32_t codeId = 0;
                    if (widened.SignedNormalized) {
                        const uint32_t asIntId =
                            emit(spv::Op::OpBitcast, normalizedMaterial.IvecTypeId,
                                 {{SPV_OPERAND_TYPE_ID, {texelId}}});
                        if (asIntId == 0u) return 0u;
                        const uint32_t shiftedUpId =
                            emit(spv::Op::OpShiftLeftLogical, normalizedMaterial.IvecTypeId,
                                 {{SPV_OPERAND_TYPE_ID, {asIntId}},
                                  {SPV_OPERAND_TYPE_ID, {normalizedMaterial.ShiftWidthId}}});
                        if (shiftedUpId == 0u) return 0u;
                        const uint32_t signExtendedId =
                            emit(spv::Op::OpShiftRightArithmetic, normalizedMaterial.IvecTypeId,
                                 {{SPV_OPERAND_TYPE_ID, {shiftedUpId}},
                                  {SPV_OPERAND_TYPE_ID, {normalizedMaterial.ShiftWidthId}}});
                        if (signExtendedId == 0u) return 0u;
                        codeId = emit(spv::Op::OpConvertSToF, normalizedMaterial.FvecTypeId,
                                      {{SPV_OPERAND_TYPE_ID, {signExtendedId}}});
                    } else {
                        codeId = emit(spv::Op::OpConvertUToF, normalizedMaterial.FvecTypeId,
                                      {{SPV_OPERAND_TYPE_ID, {texelId}}});
                    }
                    if (codeId == 0u) return 0u;
                    const uint32_t normalizedId =
                        emit(spv::Op::OpFDiv, normalizedMaterial.FvecTypeId,
                             {{SPV_OPERAND_TYPE_ID, {codeId}}, {SPV_OPERAND_TYPE_ID, {widened.DenominatorId}}});
                    if (normalizedId == 0u || !widened.SignedNormalized) return normalizedId;
                    // -2^(b-1) is representable in the code but GL clamps it to -1: the signed
                    // decode is max(c / (2^(b-1) - 1), -1), not the bare division.
                    return emit(spv::Op::OpExtInst, normalizedMaterial.FvecTypeId,
                                {{SPV_OPERAND_TYPE_ID, {normalizedMaterial.Glsl450Id}},
                                 {SPV_OPERAND_TYPE_EXTENSION_INSTRUCTION_NUMBER, {kGlslFMax}},
                                 {SPV_OPERAND_TYPE_ID, {normalizedId}},
                                 {SPV_OPERAND_TYPE_ID, {normalizedMaterial.MinusOneId}}});
                };

                auto insertPack = [&](Instruction* before, const WidenedImage& widened,
                                      uint32_t valueId) -> uint32_t {
                    const auto emit = [&](spv::Op opcode, uint32_t typeId,
                                          Instruction::OperandList operands) -> uint32_t {
                        const uint32_t resultId = irContext->TakeNextId();
                        if (resultId == 0u) return 0u;
                        before->InsertBefore(spvtools::MakeUnique<Instruction>(irContext, opcode, typeId, resultId,
                                                                               Move(operands)));
                        return resultId;
                    };
                    const uint32_t lowBoundId =
                        widened.SignedNormalized ? normalizedMaterial.MinusOneId : normalizedMaterial.ZeroId;
                    const uint32_t clampedId =
                        emit(spv::Op::OpExtInst, normalizedMaterial.FvecTypeId,
                             {{SPV_OPERAND_TYPE_ID, {normalizedMaterial.Glsl450Id}},
                              {SPV_OPERAND_TYPE_EXTENSION_INSTRUCTION_NUMBER, {kGlslFClamp}},
                              {SPV_OPERAND_TYPE_ID, {valueId}},
                              {SPV_OPERAND_TYPE_ID, {lowBoundId}},
                              {SPV_OPERAND_TYPE_ID, {normalizedMaterial.OneId}}});
                    if (clampedId == 0u) return 0u;
                    const uint32_t scaledId =
                        emit(spv::Op::OpFMul, normalizedMaterial.FvecTypeId,
                             {{SPV_OPERAND_TYPE_ID, {clampedId}},
                              {SPV_OPERAND_TYPE_ID, {widened.DenominatorId}}});
                    if (scaledId == 0u) return 0u;
                    uint32_t biasId = normalizedMaterial.HalfId;
                    if (widened.SignedNormalized) {
                        // 0.5 * sign(x), so the truncation below rounds a tie away from zero on
                        // both sides. sign(0) is 0, which leaves an exact zero exactly zero.
                        const uint32_t signId = emit(spv::Op::OpExtInst, normalizedMaterial.FvecTypeId,
                                                     {{SPV_OPERAND_TYPE_ID, {normalizedMaterial.Glsl450Id}},
                                                      {SPV_OPERAND_TYPE_EXTENSION_INSTRUCTION_NUMBER, {kGlslFSign}},
                                                      {SPV_OPERAND_TYPE_ID, {scaledId}}});
                        if (signId == 0u) return 0u;
                        biasId = emit(spv::Op::OpFMul, normalizedMaterial.FvecTypeId,
                                      {{SPV_OPERAND_TYPE_ID, {signId}},
                                       {SPV_OPERAND_TYPE_ID, {normalizedMaterial.HalfId}}});
                        if (biasId == 0u) return 0u;
                    }
                    const uint32_t roundedId =
                        emit(spv::Op::OpFAdd, normalizedMaterial.FvecTypeId,
                             {{SPV_OPERAND_TYPE_ID, {scaledId}}, {SPV_OPERAND_TYPE_ID, {biasId}}});
                    if (roundedId == 0u) return 0u;
                    if (!widened.SignedNormalized) {
                        return emit(spv::Op::OpConvertFToU, normalizedMaterial.UvecTypeId,
                                    {{SPV_OPERAND_TYPE_ID, {roundedId}}});
                    }
                    const uint32_t signedCodeId = emit(spv::Op::OpConvertFToS, normalizedMaterial.IvecTypeId,
                                                       {{SPV_OPERAND_TYPE_ID, {roundedId}}});
                    if (signedCodeId == 0u) return 0u;
                    const uint32_t asUintId = emit(spv::Op::OpBitcast, normalizedMaterial.UvecTypeId,
                                                   {{SPV_OPERAND_TYPE_ID, {signedCodeId}}});
                    if (asUintId == 0u) return 0u;
                    return emit(spv::Op::OpBitwiseAnd, normalizedMaterial.UvecTypeId,
                                {{SPV_OPERAND_TYPE_ID, {asUintId}},
                                 {SPV_OPERAND_TYPE_ID, {normalizedMaterial.LowWordMaskId}}});
                };

                // Masks first, while every image type still carries its ORIGINAL format: the
                // rewrite below only touches the format operand, so the accesses' types do not
                // move and the order is free either way - but doing it first keeps a failed
                // resolve from leaving a half-widened module behind.
                for (Instruction* write : writes) {
                    const WidenedImage* widened = widenedOf(write);
                    if (widened == nullptr) continue;
                    const Bool normalized = widened->DenominatorId != 0u;
                    // A carrier with as many channels as the original (rgb10_a2ui in rgba16ui) has
                    // no surplus channel to pin, and the shuffle would select (0, 1, 2, 3) from the
                    // texel - an identity the emitter would still print. Left out entirely, unless
                    // the texel still has to be PACKED, in which case the store is rewritten
                    // anyway and only the shuffle is skipped.
                    if (widened->Channels >= 4 && !normalized) continue;
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
                    uint32_t storedId = texelId;
                    if (widened->Channels < 4) {
                        const uint32_t maskedId = irContext->TakeNextId();
                        if (maskedId == 0) return Status::Failure;
                        Instruction::OperandList shuffleOperands{{SPV_OPERAND_TYPE_ID, {texelId}},
                                                                 {SPV_OPERAND_TYPE_ID, {zeroOneId}}};
                        for (const Operand& component : maskComponents(widened->Channels)) {
                            shuffleOperands.push_back(component);
                        }
                        write->InsertBefore(spvtools::MakeUnique<Instruction>(
                            irContext, spv::Op::OpVectorShuffle, vec4TypeId, maskedId, shuffleOperands));
                        storedId = maskedId;
                    }
                    // The pack goes AFTER the mask, so the carrier's surplus channels are written
                    // as the codes GL's own 0 and 1 quantise to (0 and the channel maximum) rather
                    // than as raw zeroes and ones - which is what a later imageLoad, and the
                    // upload that seeds an untouched level, both have to agree with.
                    if (normalized) {
                        storedId = insertPack(write, *widened, storedId);
                        if (storedId == 0u) return Status::Failure;
                    }
                    write->SetInOperand(kImageWriteTexelOperand, {storedId});
                }

                for (Instruction* read : reads) {
                    const WidenedImage* widened = widenedOf(read);
                    if (widened == nullptr) continue;
                    const Bool normalized = widened->DenominatorId != 0u;
                    if (widened->Channels >= 4 && !normalized) continue; // see the store loop
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
                    //
                    // Under a normalized carrier the copy reads a uvec4 rather than the vec4 the
                    // shader asked for - that is the whole point of the carrier - and the unpack
                    // in between brings it back. Every extra instruction is inserted in front of
                    // the original too, so the chain stays in order.
                    const uint32_t rawReadId = irContext->TakeNextId();
                    if (rawReadId == 0) return Status::Failure;
                    Instruction::OperandList readOperands;
                    for (uint32_t i = 0; i < read->NumInOperands(); ++i) {
                        readOperands.push_back(read->GetInOperand(i));
                    }
                    read->InsertBefore(spvtools::MakeUnique<Instruction>(
                        irContext, spv::Op::OpImageRead,
                        normalized ? normalizedMaterial.UvecTypeId : vec4TypeId, rawReadId, readOperands));
                    uint32_t loadedId = rawReadId;
                    if (normalized) {
                        loadedId = insertUnpack(read, *widened, rawReadId);
                        if (loadedId == 0u) return Status::Failure;
                    }
                    // Always a shuffle, even at four channels: it is what carries the ORIGINAL
                    // result id, which every existing use still names. At four channels the
                    // selectors are the identity (0, 1, 2, 3), so nothing is substituted.
                    read->SetOpcode(spv::Op::OpVectorShuffle);
                    Instruction::OperandList shuffleOperands{{SPV_OPERAND_TYPE_ID, {loadedId}},
                                                             {SPV_OPERAND_TYPE_ID, {zeroOneId}}};
                    for (const Operand& component : maskComponents(widened->Channels)) {
                        shuffleOperands.push_back(component);
                    }
                    read->SetInOperands(Move(shuffleOperands));
                }

                // The declaration itself, last. For most carriers only the format operand moves:
                // the carrier has the same component type as the original by construction, so the
                // OpTypeImage's Sampled Type still agrees with it (which is what spirv-val checks)
                // and no pointer, array or access-chain type has to be rebuilt.
                //
                // A NORMALIZED carrier moves the Sampled Type too - float32 to uint32, which is
                // what turns an image2D into a uimage2D - and it has to, because spirv-val
                // requires the Sampled Type to match the format's component class. Nothing above
                // the OpTypeImage has to change with it: the pointer, the variable and every
                // OpLoad name the image type by ID, and the id is being mutated in place.
                //
                // Two image types can COLLIDE here - `layout(rg32f)` and `layout(rgba32f)` in one
                // module both become Rgba32f, and so do `layout(rgba16)` image2D and
                // `layout(rgba16ui)` uimage2D - and duplicate non-aggregate type declarations are
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
                    if (widenedIt->second.DenominatorId != 0u) {
                        type->SetInOperand(kImageSampledTypeOperand, {normalizedMaterial.UintTypeId});
                    }
                }

                // StorageImageExtendedFormats is deliberately left declared even though every
                // remaining format is now one of the thirteen that need no capability: a
                // capability a module no longer exercises is valid SPIR-V, and dropping one is
                // only safe after proving no extended format is left ANYWHERE, including in image
                // types this pass declined.
                irContext->InvalidateAnalysesExceptFor(IRContext::kAnalysisNone);
                return Status::SuccessWithChange;
            }

            spvtools::Optimizer::PassToken WidenImageFormatsPass::CreateWidenImageFormatsPass(
                const bool onlyFormatsSpirvCrossRefusesToPrint) {
                return spvtools::Optimizer::PassToken(
                    spvtools::MakeUnique<WidenImageFormatsPass>(onlyFormatsSpirvCrossRefusesToPrint));
            }
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
