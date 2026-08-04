// MobileGL - MobileGL/MG_Util/ShaderTranspiler/ShaderCompiler.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#define SPV_ENABLE_UTILITY_CODE
#include "glslang/SPIRV/spirv.hpp11"
#undef SPV_ENABLE_UTILITY_CODE

#include "ShaderCompiler.h"

#include "SpirvPasses/EliminateFloatEqualsZeroPass.h"
#include "SpirvPasses/FlattenInterfaceStructPass.h"
#include "SpirvPasses/RenameSamplerFunctionParameterPass.h"
#include "SpirvPasses/DecomposeWorkgroupVec3Pass.h"
#include "SpirvPasses/DecoratePositionInvariantPass.h"
#include "SpirvPasses/LowerDrawParametersPass.h"
#include "SpirvPasses/RebaseInstanceIndexPass.h"
#include "SpirvPasses/NormalizeRectCoordinatesPass.h"
#include "SpirvPasses/StripUboMemberRelaxedPrecisionPass.h"
#include "SpirvPasses/StripNoPerspectivePass.h"
#include "SpirvPasses/EmulateNoPerspectivePass.h"
#include "spirv-tools/libspirv.h"
#include "spirv-tools/optimizer.hpp"

#include "ShaderSourceProcessor.h"
#include <MG_Backend/BackendObjects.h>
#include <MG_Util/Converters/GLToStr/GLEnumConverter.h>
#include <MG_Util/Converters/GLToGlslang/ProgramEnumConverter.h>
#include <cstdlib>

namespace MobileGL {
    namespace MG_Util {
        namespace ShaderTranspiler {
            TBuiltInResource BuildTBuiltInResource() {
                TBuiltInResource Resources{};
                Resources.maxLights = 32;
                Resources.maxClipPlanes = 6;
                Resources.maxTextureUnits = 32;
                Resources.maxTextureCoords = 32;
                Resources.maxVertexAttribs = 64;
                Resources.maxVertexUniformComponents = 4096;
                Resources.maxVaryingFloats = 64;
                Resources.maxVertexTextureImageUnits = 32;
                Resources.maxCombinedTextureImageUnits = 80;
                Resources.maxTextureImageUnits = 32;
                Resources.maxFragmentUniformComponents = 4096;
                Resources.maxDrawBuffers = 32;
                Resources.maxVertexUniformVectors = 128;
                Resources.maxVaryingVectors = 8;
                Resources.maxFragmentUniformVectors = 256;
                Resources.maxVertexOutputVectors = 16;
                Resources.maxFragmentInputVectors = 15;
                Resources.minProgramTexelOffset = -8;
                Resources.maxProgramTexelOffset = 7;
                Resources.maxClipDistances = 8;
                Resources.maxComputeWorkGroupCountX = 65535;
                Resources.maxComputeWorkGroupCountY = 65535;
                Resources.maxComputeWorkGroupCountZ = 65535;
                Resources.maxComputeWorkGroupSizeX = 1024;
                Resources.maxComputeWorkGroupSizeY = 1024;
                // TODO: Drive glslang compute resource limits from the active backend instead of this permissive cap.
                Resources.maxComputeWorkGroupSizeZ = 1024;
                Resources.maxComputeUniformComponents = 1024;
                Resources.maxComputeTextureImageUnits = 16;
                Resources.maxComputeImageUniforms = 8;
                Resources.maxComputeAtomicCounters = 8;
                Resources.maxComputeAtomicCounterBuffers = 1;
                Resources.maxVaryingComponents = 60;
                Resources.maxVertexOutputComponents = 64;
                Resources.maxGeometryInputComponents = 64;
                Resources.maxGeometryOutputComponents = 128;
                Resources.maxFragmentInputComponents = 128;
                Resources.maxImageUnits = 8;
                Resources.maxCombinedImageUnitsAndFragmentOutputs = 8;
                Resources.maxCombinedShaderOutputResources = 8;
                Resources.maxImageSamples = 0;
                Resources.maxVertexImageUniforms = 0;
                Resources.maxTessControlImageUniforms = 0;
                Resources.maxTessEvaluationImageUniforms = 0;
                Resources.maxGeometryImageUniforms = 0;
                Resources.maxFragmentImageUniforms = 8;
                Resources.maxCombinedImageUniforms = 8;
                Resources.maxGeometryTextureImageUnits = 16;
                Resources.maxGeometryOutputVertices = 256;
                Resources.maxGeometryTotalOutputComponents = 1024;
                Resources.maxGeometryUniformComponents = 1024;
                Resources.maxGeometryVaryingComponents = 64;
                Resources.maxTessControlInputComponents = 128;
                Resources.maxTessControlOutputComponents = 128;
                Resources.maxTessControlTextureImageUnits = 16;
                Resources.maxTessControlUniformComponents = 1024;
                Resources.maxTessControlTotalOutputComponents = 4096;
                Resources.maxTessEvaluationInputComponents = 128;
                Resources.maxTessEvaluationOutputComponents = 128;
                Resources.maxTessEvaluationTextureImageUnits = 16;
                Resources.maxTessEvaluationUniformComponents = 1024;
                Resources.maxTessPatchComponents = 120;
                Resources.maxPatchVertices = 32;
                Resources.maxTessGenLevel = 64;
                Resources.maxViewports = 16;
                Resources.maxVertexAtomicCounters = 0;
                Resources.maxTessControlAtomicCounters = 0;
                Resources.maxTessEvaluationAtomicCounters = 0;
                Resources.maxGeometryAtomicCounters = 0;
                Resources.maxFragmentAtomicCounters = 8;
                Resources.maxCombinedAtomicCounters = 8;
                Resources.maxAtomicCounterBindings = 1;
                Resources.maxVertexAtomicCounterBuffers = 0;
                Resources.maxTessControlAtomicCounterBuffers = 0;
                Resources.maxTessEvaluationAtomicCounterBuffers = 0;
                Resources.maxGeometryAtomicCounterBuffers = 0;
                Resources.maxFragmentAtomicCounterBuffers = 1;
                Resources.maxCombinedAtomicCounterBuffers = 1;
                Resources.maxAtomicCounterBufferSize = 16384;
                Resources.maxTransformFeedbackBuffers = 4;
                Resources.maxTransformFeedbackInterleavedComponents = 64;
                Resources.maxCullDistances = 8;
                Resources.maxCombinedClipAndCullDistances = 8;
                Resources.maxSamples = 4;
                Resources.maxMeshOutputVerticesNV = 256;
                Resources.maxMeshOutputPrimitivesNV = 512;
                Resources.maxMeshWorkGroupSizeX_NV = 32;
                Resources.maxMeshWorkGroupSizeY_NV = 1;
                Resources.maxMeshWorkGroupSizeZ_NV = 1;
                Resources.maxTaskWorkGroupSizeX_NV = 32;
                Resources.maxTaskWorkGroupSizeY_NV = 1;
                Resources.maxTaskWorkGroupSizeZ_NV = 1;
                Resources.maxMeshViewCountNV = 4;

                // Resource checking must describe the same backend contract exposed through
                // glGetIntegerv. Keeping this copy local also avoids racing on a process-global
                // TBuiltInResource when Iris compiles shaders concurrently.
                const MG_Backend::DynamicBackendParameters fallbackParameters{};
                const auto& activeBackend = MG_Backend::pActiveBackendObject;
                const auto& dynamicParameters =
                    activeBackend ? activeBackend->GetDynamicParameters() : fallbackParameters;
                Resources.maxImageUnits = dynamicParameters.MaxImageUnits;
                Resources.maxCombinedImageUnitsAndFragmentOutputs =
                    dynamicParameters.MaxImageUnits + dynamicParameters.MaxDrawBuffers;
                Resources.maxVertexImageUniforms = dynamicParameters.MaxVertexImageUniforms;
                Resources.maxGeometryImageUniforms = dynamicParameters.MaxGeometryImageUniforms;
                Resources.maxFragmentImageUniforms = dynamicParameters.MaxFragmentImageUniforms;
                Resources.maxComputeImageUniforms = dynamicParameters.MaxComputeImageUniforms;
                Resources.maxCombinedImageUniforms = dynamicParameters.MaxCombinedImageUniforms;

                Resources.limits.nonInductiveForLoops = true;
                Resources.limits.whileLoops = true;
                Resources.limits.doWhileLoops = true;
                Resources.limits.generalUniformIndexing = true;
                Resources.limits.generalAttributeMatrixVectorIndexing = true;
                Resources.limits.generalVaryingIndexing = true;
                Resources.limits.generalSamplerIndexing = true;
                Resources.limits.generalVariableIndexing = true;
                Resources.limits.generalConstantMatrixVectorIndexing = true;

                return Resources;
            }

            // One parse attempt. A glslang::TShader cannot be re-parsed, so a retry has to build a
            // fresh one with byte-identical setup - hence a single factored body rather than two
            // copies that could drift apart.
            static Result<SharedPtr<glslang::TShader>> ParseShaderSource(EShLanguage lang, GLenum shaderType,
                                                                         const String& source,
                                                                         Flags<ShaderCompileBits> flags) {
                SharedPtr<glslang::TShader> res;
                auto& tshader = res;
                tshader = MakeShared<glslang::TShader>(lang);
                // setStrings gets no length array, so it relies on NUL termination: source must be an
                // owning buffer that outlives parse(), never a StringView's substring.
                const char* src[] = {source.c_str()};
                tshader->setStrings(src, 1);
                tshader->setNanMinMaxClamp(true);
                tshader->setInvertY(true);
                tshader->setPreamble("#undef VULKAN\n");
                if (flags & ShaderCompileBits::CompileForOpenGL) {
                    tshader->setEnvInput(glslang::EShSourceGlsl, lang, glslang::EShClientVulkan, 450);
                    tshader->setEnvClient(glslang::EShClientOpenGL, glslang::EShTargetOpenGL_450);
                    tshader->setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);
                } else {
                    tshader->setEnvInput(glslang::EShSourceGlsl, lang, glslang::EShClientVulkan, 450);
                    // MobileGL runtime currently creates Vulkan 1.1 instance/device on Android path,
                    // so generated SPIR-V must not exceed SPIR-V 1.3.
                    tshader->setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
                    tshader->setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);
                    tshader->setEnvInputVulkanRulesRelaxed(); // using EXT_vulkan_glsl_relaxed for gl_VertexID and
                                                              // gl_InstanceID?
                }
                tshader->setAutoMapLocations(true);
                tshader->setAutoMapBindings(true);
                tshader->setGlobalUniformBlockName(GLOBAL_UBO_NAME);
                auto resources = BuildTBuiltInResource();
                if (!tshader->parse(&resources, 460, ECoreProfile,
                                    /*forceDefaultVersionAndProfile: */ false,
                                    /*forwardCompatible: */ true, EShMsgDefault)) {
                    ResultInfo r;
                    r.log += "Error: [glslang] Cannot compile " + ConvertGLEnumToString(shaderType) + ":\n" +
                             std::string(tshader->getInfoLog());
                    r.errc = -2;
                    return std::unexpected(r);
                }

                return res;
            }

            Result<SharedPtr<glslang::TShader>> ShaderCompiler::CompileShader(const ShaderAttrib& attrib) {
                auto shaderType = attrib.shaderType;

                auto lang = MG_Util::ConvertGLEnumToEShLanguage(shaderType);
                if (lang == EShLanguage::EShLangCount) {
                    ResultInfo r;
                    r.log += "Error: [Preprocess] Unsupported shader type: " + ConvertGLEnumToString(shaderType);
                    r.errc = -1;
                    return std::unexpected(r);
                }

                const String source(attrib.sourceStr);
                auto result = ParseShaderSource(lang, shaderType, source, attrib.flags);
                if (result) return result;

                // Legacy desktop sources are normalized to "#version 330 core" (with a marker on the
                // directive), which parses under stricter rules than the 460 they used to be forced
                // to: a shader declaring 110-150 while using e.g. layout(binding=...) without the
                // matching #extension line compiles on real drivers but is rejected here. Retry once
                // at 460 before reporting failure; a genuinely broken shader fails both attempts and
                // keeps its original diagnostics. Application-declared 330+ sources carry no marker
                // and keep their declared version's strict rules (the GL CTS negative-compile cases
                // depend on that).
                String retrySource = source;
                if (!MG_Util::ShaderTranspiler::RetargetLegacyVersionDirectiveTo460(retrySource)) {
                    return result;
                }

                auto retryResult = ParseShaderSource(lang, shaderType, retrySource, attrib.flags);
                if (!retryResult) return result;

                MGLOG_D("CompileShader: %s only parsed after retargeting its legacy #version to 460",
                        ConvertGLEnumToString(shaderType).c_str());
                return retryResult;
            }

            Result<SharedPtr<glslang::TProgram>> ShaderCompiler::LinkProgram(const ProgramAttrib& attrib) {
                SharedPtr<glslang::TProgram> program = MakeShared<glslang::TProgram>();
                for (auto& s : attrib.shaders) {
                    program->addShader(s.get());
                }

                if (!program->link(EShMsgDefault)) {
                    ResultInfo r;
                    r.log = "Error: [glslang] Cannot link the program:\n" + std::string(program->getInfoLog());
                    r.errc = -3;
                    return std::unexpected(r);
                }

                for (const auto& [name, loc] : attrib.explicitVertexInLocations) {
                    MGLOG_D("%s: got explicitly set - layout(location = %d) %s;", __func__, loc, name.c_str());
                }

                // UniquePtr<glslang::TIoMapResolver> resolver;
                UniquePtr<TMglGlslIoResolver> resolver;
                for (unsigned stage = 0; stage < EShLangCount; stage++) {
                    if (program->getIntermediate((EShLanguage)stage) == nullptr) continue;
                    resolver =
                        MakeUnique<TMglGlslIoResolver>(*program, (EShLanguage)stage, attrib.explicitVertexInLocations,
                                                       attrib.explicitFragmentOutLocations,
                                                       attrib.explicitFragmentOutIndices,
                                                       attrib.explicitOpaqueUniformBindings);
                    break;
                }
                auto ioMapper = UniquePtr<glslang::TIoMapper>(glslang::GetGlslIoMapper());

                if (!program->mapIO(resolver.get(), ioMapper.get())) {
                    ResultInfo r;
                    r.log = "Error: [glslang] Cannot mapIO:\n" + std::string(program->getInfoLog());
                    r.errc = -4;
                    return std::unexpected(r);
                }

                return program;
            }

            Result<Vector<Vector<unsigned>>> ShaderCompiler::GetSpirvBinaryFromProgram(
                const ProgramBinaryAttrib& attrib) {
                glslang::SpvOptions spvOptions;
                spvOptions.disableOptimizer = false;

                Vector<Vector<unsigned>> allSpirv;
                for (auto type : attrib.shaderTypes) {
                    Vector<unsigned> spirv;
                    GlslangToSpv(*attrib.program.getIntermediate(ConvertGLEnumToEShLanguage(type)), spirv, &spvOptions);
                    allSpirv.push_back(spirv);
                }

                return allSpirv;
            }

            bool ShaderCompiler::SanitizeAndOptimizeBinary(const Vector<Uint32>& inputBinary,
                                                           Vector<uint32_t>& outputBinary) {
                using namespace spvtools;
                OptimizerOptions options;
                options.set_run_validator(false);

                Optimizer optimizer(SPV_ENV_VULKAN_1_1);

                optimizer.RegisterPass(CreateAggressiveDCEPass(false));
                optimizer.RegisterPass(CreateRemoveUnusedInterfaceVariablesPass());
                optimizer.RegisterPass(FlattenInterfaceStructPass::CreateFlattenInterfaceStructPass());
                optimizer.RegisterPass(RenameSamplerFunctionParameterPass::CreateRenameSamplerFunctionParameterPass());
                optimizer.RegisterPass(EliminateFloatEqualsZeroPass::CreateEliminateFloatEqualsZeroPass());
                optimizer.RegisterPass(DecomposeWorkgroupVec3Pass::CreateDecomposeWorkgroupVec3Pass());

                return optimizer.Run(inputBinary.data(), inputBinary.size(), &outputBinary, options);
            }

            bool ShaderCompiler::LowerDrawParametersForEssl(const Vector<Uint32>& inputBinary,
                                                            Vector<uint32_t>& outputBinary) {
                using namespace spvtools;
                OptimizerOptions options;
                options.set_run_validator(false);

                Optimizer optimizer(SPV_ENV_VULKAN_1_1);
                optimizer.RegisterPass(LowerDrawParametersPass::CreateLowerDrawParametersPass());

                return optimizer.Run(inputBinary.data(), inputBinary.size(), &outputBinary, options);
            }

            bool ShaderCompiler::StripUboMemberRelaxedPrecisionForEssl(const Vector<Uint32>& inputBinary,
                                                                       Vector<uint32_t>& outputBinary) {
                using namespace spvtools;
                OptimizerOptions options;
                options.set_run_validator(false);

                Optimizer optimizer(SPV_ENV_VULKAN_1_1);
                optimizer.RegisterPass(
                    StripUboMemberRelaxedPrecisionPass::CreateStripUboMemberRelaxedPrecisionPass());

                return optimizer.Run(inputBinary.data(), inputBinary.size(), &outputBinary, options);
            }

            bool ShaderCompiler::StripNoPerspectiveForEssl(const Vector<Uint32>& inputBinary,
                                                           Vector<uint32_t>& outputBinary) {
                using namespace spvtools;
                OptimizerOptions options;
                options.set_run_validator(false);

                Optimizer optimizer(SPV_ENV_VULKAN_1_1);
                optimizer.RegisterPass(StripNoPerspectivePass::CreateStripNoPerspectivePass());

                return optimizer.Run(inputBinary.data(), inputBinary.size(), &outputBinary, options);
            }

            bool ShaderCompiler::EmulateNoPerspectiveForEssl(const Vector<Uint32>& inputBinary,
                                                             Vector<uint32_t>& outputBinary) {
                using namespace spvtools;
                OptimizerOptions options;
                options.set_run_validator(false);

                Optimizer optimizer(SPV_ENV_VULKAN_1_1);
                optimizer.RegisterPass(EmulateNoPerspectivePass::CreateEmulateNoPerspectivePass());

                return optimizer.Run(inputBinary.data(), inputBinary.size(), &outputBinary, options);
            }

            bool ShaderCompiler::LowerRectImages(const Vector<Uint32>& inputBinary,
                                                 Vector<uint32_t>& outputBinary) {
                using namespace spvtools;
                OptimizerOptions options;
                options.set_run_validator(false);

                Optimizer optimizer(SPV_ENV_VULKAN_1_1);
                optimizer.RegisterPass(NormalizeRectCoordinatesPass::CreateNormalizeRectCoordinatesPass());

                return optimizer.Run(inputBinary.data(), inputBinary.size(), &outputBinary, options);
            }

            bool ShaderCompiler::RebaseInstanceIndexForVulkan(const Vector<Uint32>& inputBinary,
                                                              Vector<uint32_t>& outputBinary) {
                using namespace spvtools;
                OptimizerOptions options;
                options.set_run_validator(false);

                Optimizer optimizer(SPV_ENV_VULKAN_1_1);
                optimizer.RegisterPass(RebaseInstanceIndexPass::CreateRebaseInstanceIndexPass());

                return optimizer.Run(inputBinary.data(), inputBinary.size(), &outputBinary, options);
            }

            bool ShaderCompiler::DecoratePositionInvariantForVulkan(const Vector<Uint32>& inputBinary,
                                                                    Vector<uint32_t>& outputBinary) {
                using namespace spvtools;
                OptimizerOptions options;
                options.set_run_validator(false);

                Optimizer optimizer(SPV_ENV_VULKAN_1_1);
                optimizer.RegisterPass(DecoratePositionInvariantPass::CreateDecoratePositionInvariantPass());

                return optimizer.Run(inputBinary.data(), inputBinary.size(), &outputBinary, options);
            }

            bool ShaderCompiler::UseUnformattedFloatStorageImagesForVulkan(
                const Vector<Uint32>& inputBinary, Vector<uint32_t>& outputBinary) {
                constexpr SizeT kSpirvHeaderWordCount = 5;
                outputBinary.clear();
                if (inputBinary.size() < kSpirvHeaderWordCount || inputBinary[0] != spv::MagicNumber) {
                    return false;
                }

                Vector<Uint32> floatTypeIds;
                Vector<Uint32> resultTypeById(inputBinary[3], 0);
                Vector<Uint32> pointerPointeeTypeById(inputBinary[3], 0);
                Bool hasReadWithoutFormatCapability = false;
                Bool hasWriteWithoutFormatCapability = false;
                SizeT capabilityInsertOffset = kSpirvHeaderWordCount;

                for (SizeT offset = kSpirvHeaderWordCount; offset < inputBinary.size();) {
                    const Uint32 instructionWord = inputBinary[offset];
                    const Uint32 wordCount = instructionWord >> 16u;
                    const auto opcode = static_cast<spv::Op>(instructionWord & 0xffffu);
                    if (wordCount == 0 || offset + wordCount > inputBinary.size()) {
                        return false;
                    }

                    if (opcode == spv::Op::OpCapability && wordCount >= 2) {
                        capabilityInsertOffset = offset + wordCount;
                        const auto capability = static_cast<spv::Capability>(inputBinary[offset + 1]);
                        hasReadWithoutFormatCapability |=
                            capability == spv::Capability::StorageImageReadWithoutFormat;
                        hasWriteWithoutFormatCapability |=
                            capability == spv::Capability::StorageImageWriteWithoutFormat;
                    } else if (opcode == spv::Op::OpTypeFloat && wordCount >= 3) {
                        floatTypeIds.push_back(inputBinary[offset + 1]);
                    } else if (opcode == spv::Op::OpTypePointer && wordCount >= 4) {
                        const Uint32 pointerTypeId = inputBinary[offset + 1];
                        if (pointerTypeId >= pointerPointeeTypeById.size()) {
                            return false;
                        }
                        pointerPointeeTypeById[pointerTypeId] = inputBinary[offset + 3];
                    }

                    bool hasResult = false;
                    bool hasResultType = false;
                    spv::HasResultAndType(opcode, &hasResult, &hasResultType);
                    if (hasResult && hasResultType && wordCount >= 3) {
                        const Uint32 resultTypeId = inputBinary[offset + 1];
                        const Uint32 resultId = inputBinary[offset + 2];
                        if (resultId >= resultTypeById.size()) {
                            return false;
                        }
                        resultTypeById[resultId] = resultTypeId;
                    }
                    offset += wordCount;
                }

                // OpImageTexelPointer is the bridge to image atomic instructions. Vulkan requires
                // those image types to retain an atomic-compatible declared format, so exclude only
                // the exact image types used by an atomic path rather than disabling formatless
                // access for unrelated float images in the same module.
                Vector<Uint32> atomicImageTypeIds;
                for (SizeT offset = kSpirvHeaderWordCount; offset < inputBinary.size();) {
                    const Uint32 instructionWord = inputBinary[offset];
                    const Uint32 wordCount = instructionWord >> 16u;
                    const auto opcode = static_cast<spv::Op>(instructionWord & 0xffffu);
                    if (opcode == spv::Op::OpImageTexelPointer && wordCount >= 6) {
                        const Uint32 imageId = inputBinary[offset + 3];
                        if (imageId >= resultTypeById.size()) {
                            return false;
                        }
                        Uint32 imageTypeId = resultTypeById[imageId];
                        if (imageTypeId < pointerPointeeTypeById.size() &&
                            pointerPointeeTypeById[imageTypeId] != 0) {
                            imageTypeId = pointerPointeeTypeById[imageTypeId];
                        }
                        if (imageTypeId != 0 &&
                            std::find(atomicImageTypeIds.begin(), atomicImageTypeIds.end(), imageTypeId) ==
                                atomicImageTypeIds.end()) {
                            atomicImageTypeIds.push_back(imageTypeId);
                        }
                    }
                    offset += wordCount;
                }

                outputBinary = inputBinary;
                Bool hasFloatStorageImage = false;
                for (SizeT offset = kSpirvHeaderWordCount; offset < outputBinary.size();) {
                    const Uint32 instructionWord = outputBinary[offset];
                    const Uint32 wordCount = instructionWord >> 16u;
                    const auto opcode = static_cast<spv::Op>(instructionWord & 0xffffu);

                    // OpTypeImage operands are: result id, sampled type, dim, depth, arrayed,
                    // multisampled, sampled, image format, and an optional access qualifier.
                    if (opcode == spv::Op::OpTypeImage && wordCount >= 9) {
                        const Uint32 imageTypeId = outputBinary[offset + 1];
                        const Uint32 sampledTypeId = outputBinary[offset + 2];
                        const Uint32 sampled = outputBinary[offset + 7];
                        const Bool hasFloatSampledType =
                            std::find(floatTypeIds.begin(), floatTypeIds.end(), sampledTypeId) != floatTypeIds.end();
                        const Bool usedByAtomic =
                            std::find(atomicImageTypeIds.begin(), atomicImageTypeIds.end(), imageTypeId) !=
                            atomicImageTypeIds.end();
                        if (sampled == 2 && hasFloatSampledType && !usedByAtomic) {
                            outputBinary[offset + 8] = static_cast<Uint32>(spv::ImageFormat::Unknown);
                            hasFloatStorageImage = true;
                        }
                    }
                    offset += wordCount;
                }

                if (!hasFloatStorageImage) {
                    return true;
                }

                Vector<Uint32> addedCapabilities;
                const Uint32 capabilityInstruction =
                    (2u << 16u) | static_cast<Uint32>(spv::Op::OpCapability);
                if (!hasReadWithoutFormatCapability) {
                    addedCapabilities.push_back(capabilityInstruction);
                    addedCapabilities.push_back(
                        static_cast<Uint32>(spv::Capability::StorageImageReadWithoutFormat));
                }
                if (!hasWriteWithoutFormatCapability) {
                    addedCapabilities.push_back(capabilityInstruction);
                    addedCapabilities.push_back(
                        static_cast<Uint32>(spv::Capability::StorageImageWriteWithoutFormat));
                }
                outputBinary.insert(outputBinary.begin() + static_cast<std::ptrdiff_t>(capabilityInsertOffset),
                                    addedCapabilities.begin(), addedCapabilities.end());
                return true;
            }

            Result<String> ShaderCompiler::DecompileShader(SpvcSession& session) {
                spvc_compiler_options options;
                session.CreateOptions(&options);

                spvc_compiler_options_set_uint(options, SPVC_COMPILER_OPTION_GLSL_VERSION, 320);
                spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_GLSL_ES, SPVC_TRUE);
                spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_GLSL_VULKAN_SEMANTICS, SPVC_FALSE);

                session.SetOptions(options);

                const char* result = nullptr;
                session.Compile(&result);

                if (!result) {
                    ResultInfo r;
                    r.log += "Failed to compile the shader to GLSL: \n";
                    r.log += session.GetLastErrorString();
                    r.errc = -5;
                    return std::unexpected(r);
                }

                std::string glsl = result;

                return glsl;
            }
        } // namespace ShaderTranspiler
    } // namespace MG_Util
} // namespace MobileGL
