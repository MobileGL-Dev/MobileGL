// MobileGL - MobileGL/MG_Util/ShaderTranspiler/ShaderCompiler.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "ShaderCompiler.h"

#include "SpirvPasses/EliminateFloatEqualsZeroPass.h"
#include "SpirvPasses/FlattenInterfaceStructPass.h"
#include "SpirvPasses/RenameSamplerFunctionParameterPass.h"
#include "spirv-tools/libspirv.h"
#include "spirv-tools/optimizer.hpp"

#include <MG_Util/Converters/GLToStr/GLEnumConverter.h>
#include <MG_Util/Converters/GLToGlslang/ProgramEnumConverter.h>

namespace MobileGL {
    namespace MG_Util {
        namespace ShaderTranspiler {
            TBuiltInResource& GetTBuiltInResourceInstance() {
                static TBuiltInResource Resources{};
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

            Result<SharedPtr<glslang::TShader>> ShaderCompiler::CompileShader(const ShaderAttrib& attrib) {
                auto shaderType = attrib.shaderType;
                auto& sourceStr = attrib.sourceStr;

                auto lang = MG_Util::ConvertGLEnumToEShLanguage(shaderType);
                if (lang == EShLanguage::EShLangCount) {
                    ResultInfo r;
                    r.log += "Error: [Preprocess] Unsupported shader type: " + ConvertGLEnumToString(shaderType);
                    r.errc = -1;
                    return std::unexpected(r);
                }

                SharedPtr<glslang::TShader> res;
                auto& tshader = res;
                tshader = MakeShared<glslang::TShader>(lang);
                const char* src[] = {sourceStr.data()};
                tshader->setStrings(src, 1);
                tshader->setNanMinMaxClamp(true);
                tshader->setInvertY(true);
                if (attrib.flags & ShaderCompileBits::CompileForOpenGL) {
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
                if (!tshader->parse(&GetTBuiltInResourceInstance(), 460, ECoreProfile,
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

                return optimizer.Run(inputBinary.data(), inputBinary.size(), &outputBinary, options);
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
