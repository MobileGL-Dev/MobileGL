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
#include "SpirvPasses/RenameBuiltinShadowingFunctionsPass.h"
#include "SpirvPasses/DecomposeWorkgroupVec3Pass.h"
#include "SpirvPasses/DecoratePositionInvariantPass.h"
#include "SpirvPasses/LowerDrawParametersPass.h"
#include "SpirvPasses/PackDoubleVertexInputsPass.h"
#include "SpirvPasses/SplitArrayVertexInputsPass.h"
#include "SpirvPasses/RebaseInstanceIndexPass.h"
#include "SpirvPasses/ZeroBaseVertexPass.h"
#include "SpirvPasses/NormalizeRectCoordinatesPass.h"
#include "SpirvPasses/PrivateToEntryLocalPass.h"
#include "SpirvPasses/StripUniformLocationsPass.h"
#include "SpirvPasses/StripUboMemberRelaxedPrecisionPass.h"
#include "SpirvPasses/StripNoPerspectivePass.h"
#include "SpirvPasses/EmulateNoPerspectivePass.h"
#include "SpirvPasses/LegalizeFragmentOutputIndexPass.h"
#include "spirv-tools/libspirv.h"
#include "spirv-tools/optimizer.hpp"
#include "source/opt/build_module.h"
#include "source/opt/ir_context.h"

#include "ShaderSourceProcessor.h"
#include <MG_Backend/BackendObjects.h>
#include <MG_Util/Async/ShaderCompilePool.h>
#include <MG_Util/Converters/GLToStr/GLEnumConverter.h>
#include <MG_Util/Converters/GLToGlslang/ProgramEnumConverter.h>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <mutex>

namespace MobileGL {
    namespace MG_Util {
        namespace ShaderTranspiler {
            // `env` is the compile-time backend snapshot; null means "resolve from the live
            // backend", which is what the standalone/test entry points do. The pipeline always
            // passes one, so a worker never reaches pActiveBackendObject through here.
            TBuiltInResource BuildTBuiltInResource(const CompileEnv* env) {
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
                    env ? env->params
                        : (activeBackend ? activeBackend->GetDynamicParameters() : fallbackParameters);
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
                                                                         Flags<ShaderCompileBits> flags,
                                                                         const CompileEnv* env) {
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
                auto resources = BuildTBuiltInResource(env);
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
                auto result = ParseShaderSource(lang, shaderType, source, attrib.flags, attrib.env);
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

                auto retryResult = ParseShaderSource(lang, shaderType, retrySource, attrib.flags, attrib.env);
                if (!retryResult) return result;

                MGLOG_D("CompileShader: %s only parsed after retargeting its legacy #version to 460",
                        ConvertGLEnumToString(shaderType).c_str());
                return retryResult;
            }

            // Namespace-level rather than a function-local static, because it has to be
            // CLEARABLE: what PrewarmBuiltins latches is not a property of this process, it is
            // a property of the built-in symbol tables glslang currently holds, and
            // glslang::FinalizeProcess() deletes those. A function-local latch survived the
            // teardown that invalidated it, so an Initialize -> Destroy -> Initialize cycle
            // came back up with the tables gone and the prewarm skipped - which is exactly the
            // serialized-first-parse stall this function exists to prevent, only now
            // unfixable for the rest of the process. Reset it from DestroyImpl.
            namespace {
                Bool g_builtinsPrewarmed = false;
            } // namespace

            void ShaderCompiler::ResetPrewarmLatch() { g_builtinsPrewarmed = false; }

            void ShaderCompiler::PrewarmBuiltins() {
                if (g_builtinsPrewarmed) return;
                g_builtinsPrewarmed = true;

                // One vertex and one fragment shader is enough: the built-in table is cached
                // per (version, spvVersion, profile, source), not per stage language, and
                // both configurations CompileShader can reach - the declared-460 path and
                // the retargeted-legacy path - resolve to the same combination here because
                // ParseShaderSource always passes 460/ECoreProfile as the default. Parsing
                // both anyway costs microseconds and keeps this honest if that ever changes.
                static constexpr const char* kPrewarmVertexSource =
                    "#version 460\nvoid main() { gl_Position = vec4(0.0); }\n";
                static constexpr const char* kPrewarmFragmentSource =
                    "#version 460\nlayout(location = 0) out vec4 c;\nvoid main() { c = vec4(0.0); }\n";
                static constexpr const char* kPrewarmLegacyVertexSource =
                    "#version 330 core\nvoid main() { gl_Position = vec4(0.0); }\n";

                const CompileEnv& env = *GetDefaultCompileEnv();
                for (const auto& [type, source] :
                     {std::pair{GL_VERTEX_SHADER, kPrewarmVertexSource},
                      std::pair{GL_FRAGMENT_SHADER, kPrewarmFragmentSource},
                      std::pair{GL_VERTEX_SHADER, kPrewarmLegacyVertexSource}}) {
                    ShaderAttrib attrib{.shaderType = static_cast<GLenum>(type),
                                        .sourceStr = source,
                                        .flags = 0,
                                        .env = &env};
                    // The result is deliberately discarded: the value is the symbol table
                    // glslang cached as a side effect. A failure here is not fatal - it just
                    // means the first real compile pays for the table, exactly as before.
                    (void)CompileShader(attrib);
                }
                // The parses above left this thread's glslang allocator pointing at the last
                // TShader's pool, and that TShader is about to be destroyed with it.
                glslang::SetThreadPoolAllocator(nullptr);
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

            // -1 unresolved, 0 off, 1 on. Resolved once from MOBILEGL_VALIDATE_SPIRV on first
            // use. A live getenv rather than an MG_Config::Features field, for the same reason
            // Config.h already exempts MOBILEGL_LOG_FILE_PATH: suites like SpirvPassTest never
            // run MobileGL::Initialize(), and every Initialize() re-runs MG_ConfigLoader::Init,
            // which would clobber a programmatic override stored in the feature table.
            static std::atomic<int> g_validateSpirv{-1};
            // Total validation failures observed this process. This latch - not the wrappers'
            // return values - is the test-lane signal: validation must never change what a
            // wrapper returns, or the validating lanes would render differently from the
            // shipping configuration (fail-open call sites would silently substitute an
            // earlier-stage module).
            static std::atomic<Uint64> g_spirvValidationFailures{0};

            namespace {
                // Test lanes (desktop/CI/WSL) validate by default; device builds do not -
                // validation costs real time per module, and on device the driver is the
                // final validator anyway. MOBILEGL_VALIDATE_SPIRV overrides in either
                // direction, using the ConfigLoader truthy rule.
                constexpr bool kValidateSpirvDefault =
#if defined(__ANDROID__)
                    false;
#else
                    true;
#endif

                bool IsTruthySpirvEnvValue(const char* value) {
                    if (value == nullptr || value[0] == '\0') {
                        return false;
                    }
                    String lowered(value);
                    for (auto& c : lowered) {
                        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    }
                    return lowered != "0" && lowered != "false";
                }

                // spirv-tools' validator lazily constructs function-local static tables on
                // its first run, which on this codebase happens on a ShaderCompilePool
                // worker. Function-local statics are destroyed in reverse construction
                // order, so those tables would die BEFORE the pool's own atexit sentinel
                // (registered at first pool use) gets to drain the workers - and a worker
                // mid-Validate would then read freed memory during process exit. Pin the
                // order instead: force the tables into existence now, then register a
                // second drain handler; being registered after the tables' destructors, it
                // runs before them.
                void PinValidatorTablesForProcessExit() {
                    static std::once_flag pinnedOnce;
                    std::call_once(pinnedOnce, [] {
                        spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_1);
                        Vector<Uint32> warmup;
                        // The module is shaped to reach BOTH lazily-constructed tables in
                        // the vendored validate_id.cpp: a type-generating operand pins
                        // InstructionCanHaveTypeOperand's allow-set, and the OpExtInst use
                        // of the TYPELESS %glsl import is the one path into
                        // InstructionRequiresTypeOperand's deny-set (its call site is
                        // guarded on a referenced def with no result type). A straight-line
                        // module without it leaves the deny-set to be built later on a pool
                        // worker, re-creating the exit-order hazard for that one table.
                        if (tools.Assemble("OpCapability Shader\n"
                                           "%glsl = OpExtInstImport \"GLSL.std.450\"\n"
                                           "OpMemoryModel Logical GLSL450\n"
                                           "OpEntryPoint GLCompute %main \"main\"\n"
                                           "OpExecutionMode %main LocalSize 1 1 1\n"
                                           "%void = OpTypeVoid\n"
                                           "%fn = OpTypeFunction %void\n"
                                           "%float = OpTypeFloat 32\n"
                                           "%c = OpConstant %float 1\n"
                                           "%main = OpFunction %void None %fn\n"
                                           "%entry = OpLabel\n"
                                           "%abs = OpExtInst %float %glsl FAbs %c\n"
                                           "OpReturn\n"
                                           "OpFunctionEnd\n",
                                           &warmup)) {
                            tools.Validate(warmup);
                        }
                        std::atexit(+[] {
                            // Flip validation off first: a validator table this warmup does
                            // not know about (a future spirv-tools bump) would still be
                            // destroyed before this handler, and workers must stop entering
                            // Validate before the drain waits for them.
                            g_validateSpirv.store(0, std::memory_order_release);
                            Async::ShaderCompilePool::StopAndDrainProcessPoolAtExit();
                        });
                    });
                }

                spvtools::MessageConsumer MakeSpirvMessageConsumer(const char* site) {
                    return [site](spv_message_level_t level, const char* /*source*/,
                                  const spv_position_t& position, const char* message) {
                        const char* text = message ? message : "";
                        switch (level) {
                            case SPV_MSG_FATAL:
                            case SPV_MSG_INTERNAL_ERROR:
                            case SPV_MSG_ERROR:
                                // MGLOG_I, deliberately: at the INFO compile level of every
                                // CI/WSL/retrace build, MGLOG_E and MGLOG_W are compiled out
                                // (Log.h orders DEBUG < WARN < ERROR < INFO) and the VUID
                                // would never reach a log.
                                MGLOG_I("[spirv] %s: %s (word index %zu)", site, text, position.index);
                                break;
                            default:
                                MGLOG_D("[spirv] %s: %s", site, text);
                                break;
                        }
                    };
                }

                // Validation is decoupled from control flow on purpose: a failure logs and
                // bumps the latch, and the caller proceeds exactly as the shipping (non-
                // validating) configuration would. Tests assert on the latch delta.
                void ValidateOrLatch(const char* site, const Vector<Uint32>& binary) {
                    if (!ShaderCompiler::SpirvValidationEnabled()) {
                        return;
                    }
                    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_1);
                    tools.SetMessageConsumer(MakeSpirvMessageConsumer(site));
                    if (!tools.Validate(binary)) {
                        MGLOG_I("[spirv] %s: produced a module that fails validation (failure #%llu)",
                                site,
                                static_cast<unsigned long long>(
                                    ShaderCompiler::NoteSpirvValidationFailure()));
                    }
                }

                // Shared tail for every Optimizer wrapper in this file. The optimizer's own
                // input validator stays off even in validating lanes, for two reasons: its
                // failure is indistinguishable from a transform failure (Optimizer::Run
                // returns false before BuildModule), and the FIRST wrapper's input is
                // glslang output that is legitimately not Vulkan-clean yet. What gets
                // validated is each wrapper's OUTPUT - the only bytes a driver can ever
                // receive. The message consumer is installed unconditionally: without one,
                // spirv-tools drops pass diagnostics on the floor.
                bool RunOptimizerChecked(const char* site, spvtools::Optimizer& optimizer,
                                         const Vector<Uint32>& inputBinary,
                                         Vector<uint32_t>& outputBinary) {
                    spvtools::OptimizerOptions options;
                    options.set_run_validator(false);
                    optimizer.SetMessageConsumer(MakeSpirvMessageConsumer(site));
                    if (!optimizer.Run(inputBinary.data(), inputBinary.size(), &outputBinary, options)) {
                        return false;
                    }
                    ValidateOrLatch(site, outputBinary);
                    return true;
                }
            } // namespace

            bool ShaderCompiler::SpirvValidationEnabled() {
                int state = g_validateSpirv.load(std::memory_order_acquire);
                if (state < 0) {
                    const char* env = std::getenv("MOBILEGL_VALIDATE_SPIRV");
                    const bool resolved = env != nullptr ? IsTruthySpirvEnvValue(env) : kValidateSpirvDefault;
                    int expected = -1;
                    g_validateSpirv.compare_exchange_strong(expected, resolved ? 1 : 0,
                                                            std::memory_order_acq_rel);
                    state = g_validateSpirv.load(std::memory_order_acquire);
                    if (state == 1) {
                        PinValidatorTablesForProcessExit();
                    }
                }
                return state == 1;
            }

            void ShaderCompiler::SetSpirvValidationEnabled(bool enabled) {
                g_validateSpirv.store(enabled ? 1 : 0, std::memory_order_release);
                if (enabled) {
                    PinValidatorTablesForProcessExit();
                }
            }

            Uint64 ShaderCompiler::NoteSpirvValidationFailure() {
                return g_spirvValidationFailures.fetch_add(1, std::memory_order_relaxed) + 1;
            }

            Uint64 ShaderCompiler::SpirvValidationFailureCount() {
                return g_spirvValidationFailures.load(std::memory_order_relaxed);
            }

            Bool ShaderCompiler::ModuleDeclaresBufferTextureSampler(const Vector<Uint32>& spirv) {
                if (spirv.empty()) {
                    // Early out rather than letting BuildModule reject it: an empty module is a
                    // stage that produced no SPIR-V, which is not a capability verdict, and the
                    // parse would push a spurious diagnostic through the message consumer first.
                    return false;
                }
                // Callers gate this on the driver LACKING buffer textures, so the module build
                // here only ever happens on a degraded driver that is about to fail the compile
                // anyway - it is not on the healthy path.
                std::unique_ptr<spvtools::opt::IRContext> context = spvtools::BuildModule(
                    SPV_ENV_VULKAN_1_1, MakeSpirvMessageConsumer("ModuleDeclaresBufferTextureSampler"),
                    spirv.data(), spirv.size());
                if (!context) {
                    // Unparseable here means unusable downstream too; let the ordinary transpile
                    // path produce the error rather than inventing a capability verdict from it.
                    return false;
                }
                for (const spvtools::opt::Instruction& type : context->types_values()) {
                    if (type.opcode() != spv::Op::OpTypeImage) {
                        continue;
                    }
                    // OpTypeImage in-operands: Sampled Type, Dim, Depth, Arrayed, MS, Sampled,
                    // Format. Dim is operand 1; Dim::Buffer is what samplerBuffer/isamplerBuffer/
                    // usamplerBuffer all lower to, whatever their sampled type - and equally what
                    // the imageBuffer family lowers to, which is correct here because SPIRV-Cross
                    // requires the same extension for those. The operand-count guard mirrors
                    // NormalizeRectCoordinatesPass, which reads the same operand.
                    if (type.NumInOperands() >= 2 &&
                        static_cast<spv::Dim>(type.GetSingleWordInOperand(1)) == spv::Dim::Buffer) {
                        return true;
                    }
                }
                return false;
            }

            bool ShaderCompiler::SanitizeAndOptimizeBinary(const Vector<Uint32>& inputBinary,
                                                           Vector<uint32_t>& outputBinary) {
                using namespace spvtools;
                Optimizer optimizer(SPV_ENV_VULKAN_1_1);

                // ADCE refuses to treat a Private global as deletable while the entry point
                // still contains any OpFunctionCall (IsLocalVar -> IsEntryPointWithNoCalls), so
                // a dead vertex input feeding a never-read Private shim used to survive the
                // whole chain (the Chocapic13 shadow.vsh mc_midTexCoord/iris_MidTex case).
                // Rewriting entry-point-owned Private variables to Function storage first
                // satisfies ADCE without inlining: over 521 real Iris modules the rewrite
                // captured 17 of the 21 extra dead interface variables exhaustive inlining
                // would, while shrinking the corpus 8% - inlining grew it 20% with a 5.3x
                // worst-case module and no additional GPU-side benefit.
                optimizer.RegisterPass(PrivateToEntryLocalPass::CreatePrivateToEntryLocalPass());
                // Keep the one-arg overload: remove_outputs must stay false, forever. Output
                // variables on the entry-point interface are ADCE's only unconditional live
                // roots; XFB capture resolves varyings by OpName after this chain, and the
                // VS-out/FS-in interface contract on both backends depends on declared outputs
                // surviving even when never stored.
                optimizer.RegisterPass(CreateAggressiveDCEPass(false));
                // Complementary to ADCE, not redundant: ADCE can never delete or delist an
                // Output (see above), so never-written outputs are trimmed from the
                // OpEntryPoint operand list here.
                optimizer.RegisterPass(CreateRemoveUnusedInterfaceVariablesPass());
                // The two module-legality repairs, so the chain's output - the bytes every
                // consumer downstream sees - is valid Vulkan SPIR-V. Rect lowering used to
                // live only in the backends; a validating lane would flag every rectangle
                // module long before the backend got the chance to fix it, and the backend
                // calls remain as no-ops on the now rect-free modules.
                optimizer.RegisterPass(NormalizeRectCoordinatesPass::CreateNormalizeRectCoordinatesPass());
                optimizer.RegisterPass(StripUniformLocationsPass::CreateStripUniformLocationsPass());
                optimizer.RegisterPass(FlattenInterfaceStructPass::CreateFlattenInterfaceStructPass());
                optimizer.RegisterPass(RenameSamplerFunctionParameterPass::CreateRenameSamplerFunctionParameterPass());
                optimizer.RegisterPass(
                    RenameBuiltinShadowingFunctionsPass::CreateRenameBuiltinShadowingFunctionsPass());
                optimizer.RegisterPass(EliminateFloatEqualsZeroPass::CreateEliminateFloatEqualsZeroPass());
                optimizer.RegisterPass(DecomposeWorkgroupVec3Pass::CreateDecomposeWorkgroupVec3Pass());

                return RunOptimizerChecked("SanitizeAndOptimizeBinary", optimizer, inputBinary,
                                           outputBinary);
            }

            bool ShaderCompiler::LowerDrawParametersForEssl(const Vector<Uint32>& inputBinary,
                                                            Vector<uint32_t>& outputBinary) {
                using namespace spvtools;
                Optimizer optimizer(SPV_ENV_VULKAN_1_1);
                optimizer.RegisterPass(LowerDrawParametersPass::CreateLowerDrawParametersPass());

                return RunOptimizerChecked("LowerDrawParametersForEssl", optimizer, inputBinary,
                                           outputBinary);
            }

            bool ShaderCompiler::SplitArrayVertexInputsForEssl(const Vector<Uint32>& inputBinary,
                                                               Vector<uint32_t>& outputBinary) {
                using namespace spvtools;
                Optimizer optimizer(SPV_ENV_VULKAN_1_1);
                optimizer.RegisterPass(SplitArrayVertexInputsPass::CreateSplitArrayVertexInputsPass());

                return RunOptimizerChecked("SplitArrayVertexInputsForEssl", optimizer, inputBinary,
                                           outputBinary);
            }

            bool ShaderCompiler::PackDoubleVertexInputsForVulkan(const Vector<Uint32>& inputBinary,
                                                                 Vector<uint32_t>& outputBinary) {
                using namespace spvtools;
                Optimizer optimizer(SPV_ENV_VULKAN_1_1);
                optimizer.RegisterPass(PackDoubleVertexInputsPass::CreatePackDoubleVertexInputsPass());

                return RunOptimizerChecked("PackDoubleVertexInputsForVulkan", optimizer, inputBinary,
                                           outputBinary);
            }

            bool ShaderCompiler::StripUboMemberRelaxedPrecisionForEssl(const Vector<Uint32>& inputBinary,
                                                                       Vector<uint32_t>& outputBinary) {
                using namespace spvtools;
                Optimizer optimizer(SPV_ENV_VULKAN_1_1);
                optimizer.RegisterPass(
                    StripUboMemberRelaxedPrecisionPass::CreateStripUboMemberRelaxedPrecisionPass());

                return RunOptimizerChecked("StripUboMemberRelaxedPrecisionForEssl", optimizer,
                                           inputBinary, outputBinary);
            }

            bool ShaderCompiler::StripNoPerspectiveForEssl(const Vector<Uint32>& inputBinary,
                                                           Vector<uint32_t>& outputBinary) {
                using namespace spvtools;
                Optimizer optimizer(SPV_ENV_VULKAN_1_1);
                optimizer.RegisterPass(StripNoPerspectivePass::CreateStripNoPerspectivePass());

                return RunOptimizerChecked("StripNoPerspectiveForEssl", optimizer, inputBinary,
                                           outputBinary);
            }

            bool ShaderCompiler::EmulateNoPerspectiveForEssl(const Vector<Uint32>& inputBinary,
                                                             Vector<uint32_t>& outputBinary) {
                using namespace spvtools;
                Optimizer optimizer(SPV_ENV_VULKAN_1_1);
                optimizer.RegisterPass(EmulateNoPerspectivePass::CreateEmulateNoPerspectivePass());

                return RunOptimizerChecked("EmulateNoPerspectiveForEssl", optimizer, inputBinary,
                                           outputBinary);
            }

            bool ShaderCompiler::LegalizeFragmentOutputIndexingForEssl(const Vector<Uint32>& inputBinary,
                                                                       Vector<uint32_t>& outputBinary) {
                using namespace spvtools;

                // Detection gates everything: a module with no dynamically indexed fragment
                // output - every shader but a handful - pays one BuildModule and is handed
                // back byte for byte, so the folding chain can never perturb a shader that
                // did not need it.
                if (!LegalizeFragmentOutputIndexPass::BinaryHasDynamicOutputIndexing(inputBinary)) {
                    outputBinary = inputBinary;
                    return true;
                }

                // Stock passes do the real work. The only bespoke member is the loop-control
                // hint the stock unroller demands (see the pass header); with it set, an index
                // derived from a loop counter - the shape of the Minecraft 26.3 OIT
                // coefficient shader and of most real ones - folds to a literal here, and the
                // fallback below never runs.
                Optimizer folder(SPV_ENV_VULKAN_1_1);
                // First, because both the unroller and the marking pass below read the
                // induction variable as an OpPhi, and glslang emits it as loads and stores of
                // a Function variable.
                folder.RegisterPass(CreateLocalMultiStoreElimPass());
                folder.RegisterPass(LegalizeFragmentOutputIndexPass::CreateMarkLoopsForUnrollPass());
                folder.RegisterPass(CreateLoopUnrollPass(true));
                // Fold the unrolled induction values into the access chains, then clear out
                // what constant conditions leave behind.
                folder.RegisterPass(CreateCCPPass());
                folder.RegisterPass(CreateSimplificationPass());
                folder.RegisterPass(CreateDeadBranchElimPass());
                folder.RegisterPass(CreateBlockMergePass());

                Vector<uint32_t> folded;
                if (!RunOptimizerChecked("LegalizeFragmentOutputIndexingForEssl.fold", folder, inputBinary,
                                         folded) ||
                    folded.empty()) {
                    // Fail open onto the fallback rather than onto the illegal module.
                    folded = inputBinary;
                }

                if (!LegalizeFragmentOutputIndexPass::BinaryHasDynamicOutputIndexing(folded)) {
                    outputBinary = folded;
                    return true;
                }

                // Genuinely dynamic (uniform-derived, non-constant trip count, ...): lower it.
                Optimizer lowerer(SPV_ENV_VULKAN_1_1);
                lowerer.RegisterPass(LegalizeFragmentOutputIndexPass::CreateLowerToConstantSwitchPass());
                // The chains the lowering replaced are dead now; remove_outputs must stay
                // false here for the same reason it does in SanitizeAndOptimizeBinary.
                lowerer.RegisterPass(CreateAggressiveDCEPass(false));

                if (!RunOptimizerChecked("LegalizeFragmentOutputIndexingForEssl.lower", lowerer, folded,
                                         outputBinary) ||
                    outputBinary.empty()) {
                    outputBinary = folded;
                    return true;
                }

                if (LegalizeFragmentOutputIndexPass::BinaryHasDynamicOutputIndexing(outputBinary)) {
                    // MGLOG_I, deliberately: MGLOG_E/W are compiled out at the INFO level every
                    // CI and retrace build uses, and this is precisely the diagnostic that has
                    // to survive to explain a shader the driver is about to reject.
                    MGLOG_I("[spirv] LegalizeFragmentOutputIndexingForEssl: a fragment output is still "
                            "indexed dynamically; a strict ES driver will reject this shader");
                }
                return true;
            }

            bool ShaderCompiler::LowerRectImages(const Vector<Uint32>& inputBinary,
                                                 Vector<uint32_t>& outputBinary) {
                using namespace spvtools;
                Optimizer optimizer(SPV_ENV_VULKAN_1_1);
                optimizer.RegisterPass(NormalizeRectCoordinatesPass::CreateNormalizeRectCoordinatesPass());

                return RunOptimizerChecked("LowerRectImages", optimizer, inputBinary, outputBinary);
            }

            bool ShaderCompiler::RebaseInstanceIndexForVulkan(const Vector<Uint32>& inputBinary,
                                                              Vector<uint32_t>& outputBinary) {
                using namespace spvtools;
                Optimizer optimizer(SPV_ENV_VULKAN_1_1);
                optimizer.RegisterPass(RebaseInstanceIndexPass::CreateRebaseInstanceIndexPass());

                return RunOptimizerChecked("RebaseInstanceIndexForVulkan", optimizer, inputBinary,
                                           outputBinary);
            }

            bool ShaderCompiler::ZeroBaseVertexForVulkan(const Vector<Uint32>& inputBinary,
                                                         Vector<uint32_t>& outputBinary) {
                using namespace spvtools;
                Optimizer optimizer(SPV_ENV_VULKAN_1_1);
                optimizer.RegisterPass(ZeroBaseVertexPass::CreateZeroBaseVertexPass());

                return RunOptimizerChecked("ZeroBaseVertexForVulkan", optimizer, inputBinary, outputBinary);
            }

            bool ShaderCompiler::DecoratePositionInvariantForVulkan(const Vector<Uint32>& inputBinary,
                                                                    Vector<uint32_t>& outputBinary) {
                using namespace spvtools;
                Optimizer optimizer(SPV_ENV_VULKAN_1_1);
                optimizer.RegisterPass(DecoratePositionInvariantPass::CreateDecoratePositionInvariantPass());

                return RunOptimizerChecked("DecoratePositionInvariantForVulkan", optimizer, inputBinary,
                                           outputBinary);
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
                // Hand-rolled word walk, so no Optimizer wrapper ever sees this rewrite;
                // check the modified module explicitly in validating lanes.
                ValidateOrLatch("UseUnformattedFloatStorageImagesForVulkan", outputBinary);
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
