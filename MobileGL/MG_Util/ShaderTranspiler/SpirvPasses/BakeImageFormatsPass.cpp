// MobileGL - MobileGL/MG_Util/ShaderTranspiler/SpirvPasses/BakeImageFormatsPass.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "BakeImageFormatsPass.h"

#include "spirv.hpp"
#include "source/opt/build_module.h"
#include "source/opt/def_use_manager.h"
#include "source/opt/instruction.h"
#include "source/opt/ir_context.h"
#include "source/opt/module.h"
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

                // OpTypeImage in-operands: 0 sampled type, 1 Dim, 2 Depth, 3 Arrayed, 4 MS,
                // 5 Sampled, 6 Format, (7 Access Qualifier - Kernel only, never present here).
                constexpr uint32_t kImageSampledTypeOperand = 0;
                constexpr uint32_t kImageSampledOperand = 5;
                constexpr uint32_t kImageFormatOperand = 6;
                // A storage image, i.e. one reached through imageLoad/imageStore rather than a
                // sampler. The only kind that has a format qualifier in any GLSL dialect.
                constexpr uint32_t kSampledStorageImage = 2;

                // OpTypePointer in-operands: 0 storage class, 1 pointee.
                constexpr uint32_t kPointerStorageClassOperand = 0;
                constexpr uint32_t kPointerPointeeOperand = 1;
                // OpTypeArray in-operands: 0 element type, 1 length.
                constexpr uint32_t kArrayElementOperand = 0;

                bool IsFormatlessStorageImageType(const Instruction* type) {
                    return type != nullptr && type->opcode() == spv::Op::OpTypeImage &&
                           type->GetSingleWordInOperand(kImageSampledOperand) == kSampledStorageImage &&
                           static_cast<spv::ImageFormat>(type->GetSingleWordInOperand(kImageFormatOperand)) ==
                               spv::ImageFormat::Unknown;
                }

                // The three component classes a format layout qualifier can have. spirv-val
                // requires the Image Format's class to agree with the OpTypeImage's Sampled
                // Type, so a bake that disagrees would produce an invalid module rather than a
                // merely wrong one.
                enum class ComponentClass { Float, SignedInt, UnsignedInt, None };

                ComponentClass ClassOfImageFormat(spv::ImageFormat format) {
                    switch (format) {
                    case spv::ImageFormat::Rgba32f:
                    case spv::ImageFormat::Rgba16f:
                    case spv::ImageFormat::R32f:
                    case spv::ImageFormat::Rgba8:
                    case spv::ImageFormat::Rgba8Snorm:
                    case spv::ImageFormat::Rg32f:
                    case spv::ImageFormat::Rg16f:
                    case spv::ImageFormat::R11fG11fB10f:
                    case spv::ImageFormat::R16f:
                    case spv::ImageFormat::Rgba16:
                    case spv::ImageFormat::Rgb10A2:
                    case spv::ImageFormat::Rg16:
                    case spv::ImageFormat::Rg8:
                    case spv::ImageFormat::R16:
                    case spv::ImageFormat::R8:
                    case spv::ImageFormat::Rgba16Snorm:
                    case spv::ImageFormat::Rg16Snorm:
                    case spv::ImageFormat::Rg8Snorm:
                    case spv::ImageFormat::R16Snorm:
                    case spv::ImageFormat::R8Snorm:
                        return ComponentClass::Float;
                    case spv::ImageFormat::Rgba32i:
                    case spv::ImageFormat::Rgba16i:
                    case spv::ImageFormat::Rgba8i:
                    case spv::ImageFormat::R32i:
                    case spv::ImageFormat::Rg32i:
                    case spv::ImageFormat::Rg16i:
                    case spv::ImageFormat::Rg8i:
                    case spv::ImageFormat::R16i:
                    case spv::ImageFormat::R8i:
                        return ComponentClass::SignedInt;
                    case spv::ImageFormat::Rgba32ui:
                    case spv::ImageFormat::Rgba16ui:
                    case spv::ImageFormat::Rgba8ui:
                    case spv::ImageFormat::R32ui:
                    case spv::ImageFormat::Rgb10a2ui:
                    case spv::ImageFormat::Rg32ui:
                    case spv::ImageFormat::Rg16ui:
                    case spv::ImageFormat::Rg8ui:
                    case spv::ImageFormat::R16ui:
                    case spv::ImageFormat::R8ui:
                        return ComponentClass::UnsignedInt;
                    default:
                        return ComponentClass::None;
                    }
                }

                ComponentClass ClassOfSampledType(IRContext* context, uint32_t sampledTypeId) {
                    const Instruction* sampledType = context->get_def_use_mgr()->GetDef(sampledTypeId);
                    if (sampledType == nullptr) return ComponentClass::None;
                    if (sampledType->opcode() == spv::Op::OpTypeFloat) return ComponentClass::Float;
                    if (sampledType->opcode() == spv::Op::OpTypeInt) {
                        // OpTypeInt in-operands: 0 width, 1 signedness.
                        return sampledType->GetSingleWordInOperand(1) != 0 ? ComponentClass::SignedInt
                                                                          : ComponentClass::UnsignedInt;
                    }
                    return ComponentClass::None;
                }

                // The image type at the end of a UniformConstant variable's type chain, plus the
                // links along the way. Anything that is not `pointer -> [array ->] image` comes
                // back with a null image and is left alone.
                struct ImageTypeChain {
                    Instruction* pointerType = nullptr; // the variable's own result type
                    Instruction* arrayType = nullptr;   // null when the variable is a single image
                    Instruction* imageType = nullptr;
                };

                ImageTypeChain ResolveImageTypeChain(IRContext* context, const Instruction& variable) {
                    ImageTypeChain chain;
                    auto* defUseMgr = context->get_def_use_mgr();
                    Instruction* pointerType = defUseMgr->GetDef(variable.type_id());
                    if (pointerType == nullptr || pointerType->opcode() != spv::Op::OpTypePointer) {
                        return chain;
                    }
                    chain.pointerType = pointerType;
                    Instruction* pointee = defUseMgr->GetDef(pointerType->GetSingleWordInOperand(kPointerPointeeOperand));
                    if (pointee != nullptr && pointee->opcode() == spv::Op::OpTypeArray) {
                        chain.arrayType = pointee;
                        pointee = defUseMgr->GetDef(pointee->GetSingleWordInOperand(kArrayElementOperand));
                    }
                    if (pointee != nullptr && pointee->opcode() == spv::Op::OpTypeImage) {
                        chain.imageType = pointee;
                    }
                    return chain;
                }

                // Instructions that consume an image VALUE (the result of an OpLoad) and need no
                // result-type change of their own: the texel type they yield is independent of
                // the format operand.
                bool ConsumesImageValueWithoutRetyping(spv::Op opcode) {
                    switch (opcode) {
                    case spv::Op::OpImageWrite:
                    case spv::Op::OpImageRead:
                    case spv::Op::OpImageSparseRead:
                    case spv::Op::OpImageQuerySize:
                    case spv::Op::OpImageQuerySizeLod:
                    case spv::Op::OpImageQuerySamples:
                    case spv::Op::OpImageQueryLevels:
                    case spv::Op::OpImageQueryFormat:
                    case spv::Op::OpImageQueryOrder:
                        return true;
                    default:
                        return false;
                    }
                }

                // Debug/annotation instructions name an id without depending on its type.
                bool IsTypeAgnosticReference(spv::Op opcode) {
                    switch (opcode) {
                    case spv::Op::OpName:
                    case spv::Op::OpMemberName:
                    case spv::Op::OpDecorate:
                    case spv::Op::OpDecorateId:
                    case spv::Op::OpDecorateString:
                    case spv::Op::OpMemberDecorate:
                    case spv::Op::OpEntryPoint:
                        return true;
                    default:
                        return false;
                    }
                }
            } // namespace

            Uint32 BakeImageFormatsPass::SpirvImageFormatFromGLInternalFormat(Uint glInternalFormat) {
                switch (glInternalFormat) {
                // The GL 4.2 image format table (core spec table 8.26), in its order. Written as
                // literals rather than through the GL headers because this lives in MG_Util,
                // which the GL frontend's enums do not reach.
                case 0x8814: /*GL_RGBA32F*/ return static_cast<Uint32>(spv::ImageFormat::Rgba32f);
                case 0x881A: /*GL_RGBA16F*/ return static_cast<Uint32>(spv::ImageFormat::Rgba16f);
                case 0x8230: /*GL_RG32F*/ return static_cast<Uint32>(spv::ImageFormat::Rg32f);
                case 0x822F: /*GL_RG16F*/ return static_cast<Uint32>(spv::ImageFormat::Rg16f);
                case 0x8C3A: /*GL_R11F_G11F_B10F*/ return static_cast<Uint32>(spv::ImageFormat::R11fG11fB10f);
                case 0x822E: /*GL_R32F*/ return static_cast<Uint32>(spv::ImageFormat::R32f);
                case 0x822D: /*GL_R16F*/ return static_cast<Uint32>(spv::ImageFormat::R16f);
                case 0x8D70: /*GL_RGBA32UI*/ return static_cast<Uint32>(spv::ImageFormat::Rgba32ui);
                case 0x8D76: /*GL_RGBA16UI*/ return static_cast<Uint32>(spv::ImageFormat::Rgba16ui);
                case 0x8D7C: /*GL_RGBA8UI*/ return static_cast<Uint32>(spv::ImageFormat::Rgba8ui);
                case 0x906F: /*GL_RGB10_A2UI*/ return static_cast<Uint32>(spv::ImageFormat::Rgb10a2ui);
                case 0x823C: /*GL_RG32UI*/ return static_cast<Uint32>(spv::ImageFormat::Rg32ui);
                case 0x823A: /*GL_RG16UI*/ return static_cast<Uint32>(spv::ImageFormat::Rg16ui);
                case 0x8238: /*GL_RG8UI*/ return static_cast<Uint32>(spv::ImageFormat::Rg8ui);
                case 0x8236: /*GL_R32UI*/ return static_cast<Uint32>(spv::ImageFormat::R32ui);
                case 0x8234: /*GL_R16UI*/ return static_cast<Uint32>(spv::ImageFormat::R16ui);
                case 0x8232: /*GL_R8UI*/ return static_cast<Uint32>(spv::ImageFormat::R8ui);
                case 0x8D82: /*GL_RGBA32I*/ return static_cast<Uint32>(spv::ImageFormat::Rgba32i);
                case 0x8D88: /*GL_RGBA16I*/ return static_cast<Uint32>(spv::ImageFormat::Rgba16i);
                case 0x8D8E: /*GL_RGBA8I*/ return static_cast<Uint32>(spv::ImageFormat::Rgba8i);
                case 0x823B: /*GL_RG32I*/ return static_cast<Uint32>(spv::ImageFormat::Rg32i);
                case 0x8239: /*GL_RG16I*/ return static_cast<Uint32>(spv::ImageFormat::Rg16i);
                case 0x8237: /*GL_RG8I*/ return static_cast<Uint32>(spv::ImageFormat::Rg8i);
                case 0x8235: /*GL_R32I*/ return static_cast<Uint32>(spv::ImageFormat::R32i);
                case 0x8233: /*GL_R16I*/ return static_cast<Uint32>(spv::ImageFormat::R16i);
                case 0x8231: /*GL_R8I*/ return static_cast<Uint32>(spv::ImageFormat::R8i);
                case 0x8058: /*GL_RGBA8*/ return static_cast<Uint32>(spv::ImageFormat::Rgba8);
                case 0x805B: /*GL_RGBA16*/ return static_cast<Uint32>(spv::ImageFormat::Rgba16);
                case 0x8059: /*GL_RGB10_A2*/ return static_cast<Uint32>(spv::ImageFormat::Rgb10A2);
                case 0x822B: /*GL_RG8*/ return static_cast<Uint32>(spv::ImageFormat::Rg8);
                case 0x822C: /*GL_RG16*/ return static_cast<Uint32>(spv::ImageFormat::Rg16);
                case 0x8229: /*GL_R8*/ return static_cast<Uint32>(spv::ImageFormat::R8);
                case 0x822A: /*GL_R16*/ return static_cast<Uint32>(spv::ImageFormat::R16);
                case 0x8F97: /*GL_RGBA8_SNORM*/ return static_cast<Uint32>(spv::ImageFormat::Rgba8Snorm);
                case 0x8F9B: /*GL_RGBA16_SNORM*/ return static_cast<Uint32>(spv::ImageFormat::Rgba16Snorm);
                case 0x8F95: /*GL_RG8_SNORM*/ return static_cast<Uint32>(spv::ImageFormat::Rg8Snorm);
                case 0x8F99: /*GL_RG16_SNORM*/ return static_cast<Uint32>(spv::ImageFormat::Rg16Snorm);
                case 0x8F94: /*GL_R8_SNORM*/ return static_cast<Uint32>(spv::ImageFormat::R8Snorm);
                case 0x8F98: /*GL_R16_SNORM*/ return static_cast<Uint32>(spv::ImageFormat::R16Snorm);
                default:
                    return static_cast<Uint32>(spv::ImageFormat::Unknown);
                }
            }

            bool BakeImageFormatsPass::IsCoreEsslImageFormat(Uint32 spirvImageFormat) {
                // GLSL ES 3.1 / 3.2, table "Image Formats". Everything else in the GL table
                // exists on ES only through GL_NV_image_formats.
                switch (static_cast<spv::ImageFormat>(spirvImageFormat)) {
                case spv::ImageFormat::Rgba32f:
                case spv::ImageFormat::Rgba16f:
                case spv::ImageFormat::R32f:
                case spv::ImageFormat::Rgba8:
                case spv::ImageFormat::Rgba8Snorm:
                case spv::ImageFormat::Rgba32i:
                case spv::ImageFormat::Rgba16i:
                case spv::ImageFormat::Rgba8i:
                case spv::ImageFormat::R32i:
                case spv::ImageFormat::Rgba32ui:
                case spv::ImageFormat::Rgba16ui:
                case spv::ImageFormat::Rgba8ui:
                case spv::ImageFormat::R32ui:
                    return true;
                default:
                    return false;
                }
            }

            bool BakeImageFormatsPass::IsSpirvCrossEsslPrintableFormat(Uint32 spirvImageFormat) {
                // Mirrors SPIRV-Cross's Compiler::is_desktop_only_format (spirv_cross.cpp), which
                // CompilerGLSL::format_to_glsl consults before printing: for ESSL output it throws
                // on these instead of emitting a token, and the throw takes the whole stage with
                // it. NOT the same set as "outside GLSL ES core" - SPIRV-Cross is happy to print
                // rg32f, rg16f and the rest of the two-channel 16/32-bit formats for ES, which
                // core ES does not have either. Kept as its own list for that reason: the
                // question here is what the emitter will do, not what the language allows.
                switch (static_cast<spv::ImageFormat>(spirvImageFormat)) {
                case spv::ImageFormat::R11fG11fB10f:
                case spv::ImageFormat::R16f:
                case spv::ImageFormat::Rgb10A2:
                case spv::ImageFormat::R8:
                case spv::ImageFormat::Rg8:
                case spv::ImageFormat::R16:
                case spv::ImageFormat::Rg16:
                case spv::ImageFormat::Rgba16:
                case spv::ImageFormat::R16Snorm:
                case spv::ImageFormat::Rg16Snorm:
                case spv::ImageFormat::Rgba16Snorm:
                case spv::ImageFormat::R8Snorm:
                case spv::ImageFormat::Rg8Snorm:
                case spv::ImageFormat::R8ui:
                case spv::ImageFormat::Rg8ui:
                case spv::ImageFormat::R16ui:
                case spv::ImageFormat::Rgb10a2ui:
                case spv::ImageFormat::R8i:
                case spv::ImageFormat::Rg8i:
                case spv::ImageFormat::R16i:
                    return false;
                case spv::ImageFormat::Unknown:
                    return false;
                default:
                    return true;
                }
            }

            String BakeImageFormatsPass::EsslSpellingOfGLInternalFormat(Uint glInternalFormat) {
                switch (static_cast<spv::ImageFormat>(SpirvImageFormatFromGLInternalFormat(glInternalFormat))) {
                case spv::ImageFormat::Rgba32f: return "rgba32f";
                case spv::ImageFormat::Rgba16f: return "rgba16f";
                case spv::ImageFormat::R32f: return "r32f";
                case spv::ImageFormat::Rgba8: return "rgba8";
                case spv::ImageFormat::Rgba8Snorm: return "rgba8_snorm";
                case spv::ImageFormat::Rg32f: return "rg32f";
                case spv::ImageFormat::Rg16f: return "rg16f";
                case spv::ImageFormat::R11fG11fB10f: return "r11f_g11f_b10f";
                case spv::ImageFormat::R16f: return "r16f";
                case spv::ImageFormat::Rgba16: return "rgba16";
                case spv::ImageFormat::Rgb10A2: return "rgb10_a2";
                case spv::ImageFormat::Rg16: return "rg16";
                case spv::ImageFormat::Rg8: return "rg8";
                case spv::ImageFormat::R16: return "r16";
                case spv::ImageFormat::R8: return "r8";
                case spv::ImageFormat::Rgba16Snorm: return "rgba16_snorm";
                case spv::ImageFormat::Rg16Snorm: return "rg16_snorm";
                case spv::ImageFormat::Rg8Snorm: return "rg8_snorm";
                case spv::ImageFormat::R16Snorm: return "r16_snorm";
                case spv::ImageFormat::R8Snorm: return "r8_snorm";
                case spv::ImageFormat::Rgba32i: return "rgba32i";
                case spv::ImageFormat::Rgba16i: return "rgba16i";
                case spv::ImageFormat::Rgba8i: return "rgba8i";
                case spv::ImageFormat::R32i: return "r32i";
                case spv::ImageFormat::Rg32i: return "rg32i";
                case spv::ImageFormat::Rg16i: return "rg16i";
                case spv::ImageFormat::Rg8i: return "rg8i";
                case spv::ImageFormat::R16i: return "r16i";
                case spv::ImageFormat::R8i: return "r8i";
                case spv::ImageFormat::Rgba32ui: return "rgba32ui";
                case spv::ImageFormat::Rgba16ui: return "rgba16ui";
                case spv::ImageFormat::Rgba8ui: return "rgba8ui";
                case spv::ImageFormat::R32ui: return "r32ui";
                case spv::ImageFormat::Rgb10a2ui: return "rgb10_a2ui";
                case spv::ImageFormat::Rg32ui: return "rg32ui";
                case spv::ImageFormat::Rg16ui: return "rg16ui";
                case spv::ImageFormat::Rg8ui: return "rg8ui";
                case spv::ImageFormat::R16ui: return "r16ui";
                case spv::ImageFormat::R8ui: return "r8ui";
                default:
                    return {};
                }
            }

            bool BakeImageFormatsPass::DeclaresFormatlessStorageImage(const Vector<Uint32>& binary) {
                std::unique_ptr<IRContext> context = spvtools::BuildModule(
                    SPV_ENV_VULKAN_1_1, [](spv_message_level_t, const char*, const spv_position_t&, const char*) {},
                    binary.data(), binary.size());
                if (!context) {
                    return false;
                }
                for (const Instruction& type : context->module()->types_values()) {
                    if (IsFormatlessStorageImageType(&type)) {
                        return true;
                    }
                }
                return false;
            }

            spvtools::opt::Pass::Status BakeImageFormatsPass::Process() {
                auto* irContext = context();
                auto* defUseMgr = irContext->get_def_use_mgr();

                // Cheap gate first: no format-less storage image type, nothing this pass can do,
                // and the module is handed back byte-identical.
                bool hasFormatlessType = false;
                for (const Instruction& type : irContext->types_values()) {
                    if (IsFormatlessStorageImageType(&type)) {
                        hasFormatlessType = true;
                        break;
                    }
                }
                if (!hasFormatlessType || m_glFormatByName.empty()) {
                    return Status::SuccessWithoutChange;
                }

                // Variable name -> OpName target, built once. OpName is how the frontend's
                // reflection and this module agree on which uniform is which; SPIRV-Cross
                // preserves it, which is also why the emitted ESSL can be matched by name later.
                std::map<uint32_t, const Instruction*> nameById;
                for (const Instruction& debugInst : irContext->debugs2()) {
                    if (debugInst.opcode() == spv::Op::OpName) {
                        nameById.emplace(debugInst.GetSingleWordInOperand(0), &debugInst);
                    }
                }

                struct Candidate {
                    Instruction* variable = nullptr;
                    ImageTypeChain chain;
                    spv::ImageFormat format = spv::ImageFormat::Unknown;
                    // The access chains and loads that reach the image through this variable,
                    // collected while validating so the mutation half never has to re-walk.
                    std::vector<Instruction*> accessChains;
                    std::vector<Instruction*> loads;
                };
                std::vector<Candidate> candidates;

                for (Instruction& global : irContext->types_values()) {
                    if (global.opcode() != spv::Op::OpVariable) continue;
                    if (static_cast<spv::StorageClass>(global.GetSingleWordInOperand(0)) !=
                        spv::StorageClass::UniformConstant) {
                        continue;
                    }
                    // An initializer would be a second operand, and the variable is moved behind
                    // its new type below - which would put it in front of that initializer.
                    // GLSL never gives a UniformConstant image one; refuse rather than reason.
                    if (global.NumInOperands() > 1) continue;
                    const ImageTypeChain chain = ResolveImageTypeChain(irContext, global);
                    if (!IsFormatlessStorageImageType(chain.imageType)) continue;

                    const auto nameIt = nameById.find(global.result_id());
                    if (nameIt == nameById.end()) continue;
                    const String uniformName = nameIt->second->GetInOperand(1).AsString();
                    const auto formatIt = m_glFormatByName.find(uniformName);
                    if (formatIt == m_glFormatByName.end()) continue;

                    const auto format =
                        static_cast<spv::ImageFormat>(SpirvImageFormatFromGLInternalFormat(formatIt->second));
                    if (format == spv::ImageFormat::Unknown) continue;
                    // SPIRV-Cross THROWS rather than prints for the formats it calls
                    // desktop-only when targeting ESSL, and a throw loses the whole stage - so
                    // baking one of those would trade a missing qualifier for a missing shader.
                    // Those formats are completed in the emitted text instead (see
                    // PrgramImpl::BakeImageFormatQualifiers); the module is left format-less for
                    // them, which is exactly the state that pass looks for.
                    if (!IsSpirvCrossEsslPrintableFormat(static_cast<Uint32>(format))) continue;
                    // spirv-val: "Expected Image Format to match Sampled Type". A bind format
                    // whose class disagrees with the declaration is an application error GL
                    // leaves undefined; baking it would turn that into an invalid module, so it
                    // is declined and the image stays format-less.
                    if (ClassOfImageFormat(format) !=
                        ClassOfSampledType(irContext,
                                           chain.imageType->GetSingleWordInOperand(kImageSampledTypeOperand))) {
                        continue;
                    }

                    Candidate candidate;
                    candidate.variable = &global;
                    candidate.chain = chain;
                    candidate.format = format;

                    // Validate every use BEFORE anything is mutated: a shape this pass cannot
                    // retype end to end has to leave the variable exactly as it found it, and a
                    // half-retyped module is not something a later decline could undo.
                    bool rewritable = true;
                    defUseMgr->ForEachUser(&global, [&](Instruction* user) {
                        if (!rewritable) return;
                        if (IsTypeAgnosticReference(user->opcode())) return;
                        if (user->opcode() == spv::Op::OpAccessChain ||
                            user->opcode() == spv::Op::OpInBoundsAccessChain) {
                            // Only an access chain that lands ON the image - i.e. whose result is
                            // a pointer to the image type this pass is about to replace.
                            const Instruction* resultType = defUseMgr->GetDef(user->type_id());
                            if (resultType == nullptr || resultType->opcode() != spv::Op::OpTypePointer ||
                                resultType->GetSingleWordInOperand(kPointerPointeeOperand) !=
                                    chain.imageType->result_id()) {
                                rewritable = false;
                                return;
                            }
                            candidate.accessChains.push_back(user);
                            return;
                        }
                        if (user->opcode() == spv::Op::OpLoad) {
                            candidate.loads.push_back(user);
                            return;
                        }
                        // OpImageTexelPointer takes the image POINTER and yields a pointer to the
                        // sampled type, which the format operand does not enter into.
                        if (user->opcode() == spv::Op::OpImageTexelPointer) return;
                        rewritable = false;
                    });
                    if (!rewritable) continue;

                    for (SizeT i = 0; i < candidate.accessChains.size() && rewritable; ++i) {
                        Instruction* accessChain = candidate.accessChains[i];
                        defUseMgr->ForEachUser(accessChain, [&](Instruction* user) {
                            if (!rewritable) return;
                            if (IsTypeAgnosticReference(user->opcode())) return;
                            if (user->opcode() == spv::Op::OpLoad) {
                                candidate.loads.push_back(user);
                                return;
                            }
                            if (user->opcode() == spv::Op::OpImageTexelPointer) return;
                            rewritable = false;
                        });
                    }
                    if (!rewritable) continue;

                    for (SizeT i = 0; i < candidate.loads.size() && rewritable; ++i) {
                        Instruction* load = candidate.loads[i];
                        if (load->type_id() != chain.imageType->result_id()) {
                            rewritable = false;
                            break;
                        }
                        defUseMgr->ForEachUser(load, [&](Instruction* user) {
                            if (!rewritable) return;
                            if (IsTypeAgnosticReference(user->opcode())) return;
                            if (ConsumesImageValueWithoutRetyping(user->opcode())) return;
                            // Everything else - an image handed to a function, copied into a
                            // local, put in a composite - would need the type change carried
                            // further than this pass reasons about.
                            rewritable = false;
                        });
                    }
                    if (!rewritable) continue;

                    candidates.push_back(Move(candidate));
                }

                if (candidates.empty()) {
                    return Status::SuccessWithoutChange;
                }

                // Type cloning. A new OpTypeImage is built by hand rather than through the type
                // manager because the manager always writes the OpTypeImage Access Qualifier
                // operand, which is a Kernel-capability operand: emitting it into a Shader module
                // is what spirv-val rejects, not what the format change needed.
                //
                // Each clone is inserted immediately AFTER the instruction it was cloned from.
                // That is what keeps the module free of forward references: everything the
                // original depended on is already defined above it, and every user of the
                // original - the OpVariable among them - is below it.
                std::map<std::pair<uint32_t, uint32_t>, uint32_t> cloneCache; // (original type, key) -> id

                auto findIdenticalType = [&](const Instruction& candidateType) -> uint32_t {
                    for (const Instruction& type : irContext->types_values()) {
                        if (type.opcode() != candidateType.opcode()) continue;
                        if (type.NumInOperands() != candidateType.NumInOperands()) continue;
                        bool same = true;
                        for (uint32_t i = 0; i < type.NumInOperands(); ++i) {
                            if (type.GetInOperand(i).words != candidateType.GetInOperand(i).words) {
                                same = false;
                                break;
                            }
                        }
                        if (same) return type.result_id();
                    }
                    return 0;
                };

                // The later of two instructions in the globals section. Both a new type's
                // ORIGINAL and the definition of the operand it was given have to precede it, and
                // the second of those can be a type the module declared further down (a join, see
                // below) - so the two are compared rather than assumed.
                auto laterInGlobals = [&](Instruction* a, Instruction* b) -> Instruction* {
                    if (a == nullptr) return b;
                    if (b == nullptr) return a;
                    Instruction* last = nullptr;
                    for (Instruction& global : irContext->types_values()) {
                        if (&global == a || &global == b) last = &global;
                    }
                    return last != nullptr ? last : a;
                };

                // Clones `original`, replacing in-operand `operandIndex` with `value`. Returns an
                // EXISTING type id when the module already declares the result: two identical
                // type declarations are invalid SPIR-V, and a module that already spells the
                // wanted image (say a second uniform declared `layout(r32ui)`) must be JOINED to
                // that declaration, not given a second one.
                //
                // Placement is the other half of staying valid. SPIR-V allows no forward
                // reference among types, so a clone goes after whichever of its original and its
                // new operand's definition comes last - the join case is exactly where those two
                // differ, and putting the clone after the original alone is what left an
                // OpVariable naming a pointer type declared below it.
                auto cloneTypeWithOperand = [&](Instruction* original, uint32_t operandIndex,
                                                uint32_t value) -> uint32_t {
                    const auto cacheKey = std::make_pair(original->result_id(), value);
                    const auto cached = cloneCache.find(cacheKey);
                    if (cached != cloneCache.end()) return cached->second;

                    std::vector<Operand> operands;
                    operands.reserve(original->NumInOperands());
                    for (uint32_t i = 0; i < original->NumInOperands(); ++i) {
                        operands.push_back(original->GetInOperand(i));
                    }

                    auto clone = spvtools::MakeUnique<Instruction>(irContext, original->opcode(), 0,
                                                                   irContext->TakeNextId(), operands);
                    if (clone->result_id() == 0) return 0;
                    clone->SetInOperand(operandIndex, {value});
                    const uint32_t existing = findIdenticalType(*clone);
                    if (existing != 0) {
                        cloneCache.emplace(cacheKey, existing);
                        return existing;
                    }
                    const uint32_t newId = clone->result_id();
                    Instruction* anchor = laterInGlobals(original, defUseMgr->GetDef(value));
                    if (anchor == nullptr) return 0;
                    Instruction* inserted = clone.release();
                    inserted->InsertAfter(anchor);
                    defUseMgr->AnalyzeInstDefUse(inserted);
                    cloneCache.emplace(cacheKey, newId);
                    return newId;
                };

                // Types the retype leaves behind. Killed at the end when nothing references them
                // any more: a stranded Unknown-format image type is legal SPIR-V but is exactly
                // the thing a later reader (this pass's own probe among them) would take for a
                // shader that still needs baking.
                std::vector<Instruction*> possiblyOrphanedTypes;

                bool changed = false;
                for (Candidate& candidate : candidates) {
                    const uint32_t newImageId = cloneTypeWithOperand(candidate.chain.imageType, kImageFormatOperand,
                                                                    static_cast<uint32_t>(candidate.format));
                    if (newImageId == 0) return Status::Failure;

                    // The pointer-to-image type every access chain and every single-image
                    // variable resolves through.
                    uint32_t newPointeeId = newImageId;
                    if (candidate.chain.arrayType != nullptr) {
                        newPointeeId = cloneTypeWithOperand(candidate.chain.arrayType, kArrayElementOperand,
                                                            newImageId);
                        if (newPointeeId == 0) return Status::Failure;
                    }
                    const uint32_t newVariablePointerId = cloneTypeWithOperand(
                        candidate.chain.pointerType, kPointerPointeeOperand, newPointeeId);
                    if (newVariablePointerId == 0) return Status::Failure;

                    candidate.variable->SetResultType(newVariablePointerId);
                    defUseMgr->AnalyzeInstUse(candidate.variable);
                    // ...and move it behind that type, for the same no-forward-reference reason.
                    // A joined type can live anywhere in the globals section, including below the
                    // variable that now names it. Safe unconditionally because the only operand a
                    // UniformConstant OpVariable can have besides its storage class is an
                    // initializer, and a candidate carrying one was refused above.
                    if (Instruction* pointerTypeInst = defUseMgr->GetDef(newVariablePointerId);
                        pointerTypeInst != nullptr) {
                        candidate.variable->RemoveFromList();
                        candidate.variable->InsertAfter(pointerTypeInst);
                    }

                    for (Instruction* accessChain : candidate.accessChains) {
                        Instruction* oldResultType = defUseMgr->GetDef(accessChain->type_id());
                        if (oldResultType == nullptr) return Status::Failure;
                        const uint32_t newResultType =
                            cloneTypeWithOperand(oldResultType, kPointerPointeeOperand, newImageId);
                        if (newResultType == 0) return Status::Failure;
                        accessChain->SetResultType(newResultType);
                        defUseMgr->AnalyzeInstUse(accessChain);
                        possiblyOrphanedTypes.push_back(oldResultType);
                    }
                    for (Instruction* load : candidate.loads) {
                        load->SetResultType(newImageId);
                        defUseMgr->AnalyzeInstUse(load);
                    }
                    // Innermost last, so the sweep below - which only kills what nothing
                    // references - can free a whole chain in one walk.
                    possiblyOrphanedTypes.push_back(candidate.chain.pointerType);
                    if (candidate.chain.arrayType != nullptr) {
                        possiblyOrphanedTypes.push_back(candidate.chain.arrayType);
                    }
                    possiblyOrphanedTypes.push_back(candidate.chain.imageType);

                    // Every format outside the thirteen the Shader capability covers - the same
                    // thirteen GLSL ES has in core, which is not a coincidence: both lists are
                    // the formats Vulkan requires without an optional feature. Baking one of the
                    // rest without declaring the capability produces a module spirv-val rejects
                    // ("Operand 8 of TypeImage requires ... StorageImageExtendedFormats"), which
                    // is how the stencil half's r8ui announced itself.
                    if (!IsCoreEsslImageFormat(static_cast<Uint32>(candidate.format))) {
                        irContext->AddCapability(spv::Capability::StorageImageExtendedFormats);
                    }
                    changed = true;
                }

                if (!changed) {
                    return Status::SuccessWithoutChange;
                }

                // The types the retype stranded. Ordered outermost-first above and swept in that
                // order, so a pointer goes before the image it pointed at and the image is
                // unreferenced by the time it is reached. Anything still referenced - by another
                // uniform this pass declined, or by an OpName - is simply left.
                for (Instruction* orphan : possiblyOrphanedTypes) {
                    if (orphan == nullptr) continue;
                    if (defUseMgr->NumUsers(orphan) != 0) continue;
                    // Later entries may name the same instruction (several candidates sharing a
                    // type); scrub the duplicates before the pointer goes stale.
                    for (Instruction*& other : possiblyOrphanedTypes) {
                        if (other == orphan) other = nullptr;
                    }
                    irContext->KillInst(orphan);
                }

                // StorageImageWriteWithoutFormat / StorageImageReadWithoutFormat are deliberately
                // left declared. A capability a module no longer exercises is valid SPIR-V, and
                // dropping one is only safe after proving no format-less image is left ANYWHERE -
                // including the ones this pass declined - which is a stronger claim than the
                // rewrite needs to make.
                irContext->InvalidateAnalysesExceptFor(IRContext::kAnalysisNone);
                return Status::SuccessWithChange;
            }

            spvtools::Optimizer::PassToken BakeImageFormatsPass::CreateBakeImageFormatsPass(
                GLFormatByName glFormatByName) {
                return spvtools::Optimizer::PassToken(
                    spvtools::MakeUnique<BakeImageFormatsPass>(Move(glFormatByName)));
            }
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
