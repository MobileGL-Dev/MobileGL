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
#include "SpirvPasses/DemoteFloat64Pass.h"
#include "SpirvPasses/FlattenFloat64StorageBlockPass.h"
#include "SpirvPasses/LowerDrawParametersPass.h"
#include "SpirvPasses/LowerViewportIndexPass.h"
#include "SpirvPasses/PackDoubleVertexInputsPass.h"
#include "SpirvPasses/FlattenXfbInterfaceBlocksPass.h"
#include "SpirvPasses/UniquifyIoBlockNamesPass.h"
#include "SpirvPasses/SplitArrayVertexInputsPass.h"
#include "SpirvPasses/RebaseInstanceIndexPass.h"
#include "SpirvPasses/ZeroBaseVertexPass.h"
#include "SpirvPasses/DeriveNumSubgroupsPass.h"
#include "SpirvPasses/EmulateSubgroupsPass.h"
#include "SpirvPasses/FixIterationRPBarrierPass.h"
#include "SpirvPasses/FixIterationRPSubgroupScratchPass.h"
#include "SpirvPasses/NormalizeRectCoordinatesPass.h"
#include "SpirvPasses/Lower1DArrayImagesPass.h"
#include "SpirvPasses/Lower1DSampledImagesPass.h"
#include "SpirvPasses/BakeImageFormatsPass.h"
#include "SpirvPasses/WidenImageFormatsPass.h"
#include "SpirvPasses/ClampMultisampleFetchPass.h"
#include "SpirvPasses/PrivateToEntryLocalPass.h"
#include "SpirvPasses/StripUniformLocationsPass.h"
#include "SpirvPasses/StripUboMemberRelaxedPrecisionPass.h"
#include "SpirvPasses/StripNoPerspectivePass.h"
#include "SpirvPasses/EmulateNoPerspectivePass.h"
#include "SpirvPasses/LegalizeFragmentOutputIndexPass.h"
#include "SpirvPasses/LegalizeResourceArrayIndexPass.h"
#include "SpirvPasses/FlattenAtomicCounterBlockPass.h"
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
                Resources.maxComputeUniformComponents = MAX_COMPUTE_UNIFORM_COMPONENTS;
                Resources.maxComputeTextureImageUnits = 16;
                Resources.maxComputeImageUniforms = 8;
                Resources.maxComputeAtomicCounters = MAX_ATOMIC_COUNTERS_PER_STAGE;
                Resources.maxComputeAtomicCounterBuffers = MAX_ATOMIC_COUNTER_BUFFERS_PER_STAGE;
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
                Resources.maxFragmentAtomicCounters = MAX_ATOMIC_COUNTERS_PER_STAGE;
                Resources.maxCombinedAtomicCounters = MAX_ATOMIC_COUNTERS_PER_STAGE;
                // Every atomic-counter limit below is the one glGetIntegerv answers; the shared
                // constants in Types.h are what keeps the two sides from drifting apart again.
                // gl_MaxAtomicCounterBindings and gl_MaxAtomicCounterBufferSize expand from these
                // (Initialize.cpp), and the binding count is also the ceiling glslang checks a
                // `layout(binding = N) uniform atomic_uint` against - it was 1, so every counter
                // outside binding 0 failed to compile.
                Resources.maxAtomicCounterBindings = MAX_ATOMIC_COUNTER_BUFFER_BINDINGS;
                Resources.maxVertexAtomicCounterBuffers = 0;
                Resources.maxTessControlAtomicCounterBuffers = 0;
                Resources.maxTessEvaluationAtomicCounterBuffers = 0;
                Resources.maxGeometryAtomicCounterBuffers = 0;
                Resources.maxFragmentAtomicCounterBuffers = MAX_ATOMIC_COUNTER_BUFFERS_PER_STAGE;
                Resources.maxCombinedAtomicCounterBuffers = MAX_ATOMIC_COUNTER_BUFFERS_PER_STAGE;
                Resources.maxAtomicCounterBufferSize = MAX_ATOMIC_COUNTER_BUFFER_SIZE;
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
                //
                // MEMO-HAZARD RULE FOR THIS BLOCK. Everything below is an env-derived value that
                // glslang enforces at parse AND expands into a built-in constant, so every one of
                // them can change the SPIR-V a module generates. EVERY LINE BELOW MUST BE HASHED
                // BY ComputeFrontendCompileEnvFingerprint(), which is the L1 shader-translation
                // memo's environment key - adding a read here without adding it there is a silent
                // miscompile, not a slow path. See the classification on
                // CompileEnv::frontendFingerprint.
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
                Resources.maxComputeTextureImageUnits = dynamicParameters.MaxComputeTextureImageUnits;
                // Load-bearing, not cosmetic. glslang rejects gl_ClipDistance[i] for
                // i >= maxClipDistances (ParseHelper.cpp) and expands gl_MaxClipDistances from the
                // same number, so tracking the backend limit is what turns "the program links,
                // the backend's shader compile fails somewhere the frontend never surfaces, and
                // the draw renders nothing" into an honest glCompileShader error with a log. It is
                // also what makes glGetIntegerv(GL_MAX_CLIP_DISTANCES) and gl_MaxClipDistances
                // agree, which KHR-GLxx.clip_distance.coverage compares directly.
                Resources.maxClipDistances = dynamicParameters.MaxClipDistances;

                // The compute work-group limits are the env's, not the backend parameters': they
                // are the only ones that come from a REAL indexed driver query, which
                // CaptureCompileEnv already issued once on the GL thread and floored at the core
                // minimum exactly as GL_Getter does. Reading the same snapshot here is what makes
                // gl_MaxComputeWorkGroupSize and glGetIntegeri_v agree by construction
                // (KHR-GL43.compute_shader.max compares them); the z component was 1024 here
                // against the 64 every ES driver reports. A null env is the standalone/test entry
                // point, which has no context to have queried one - the core minimums stand, which
                // is what a default-constructed CompileEnv carries anyway.
                const Uint* maxWorkGroupSize = env ? env->maxComputeWorkGroupSize : MIN_COMPUTE_WORK_GROUP_SIZE;
                const Uint* maxWorkGroupCount = env ? env->maxComputeWorkGroupCount : MIN_COMPUTE_WORK_GROUP_COUNT;
                Resources.maxComputeWorkGroupSizeX = static_cast<int>(maxWorkGroupSize[0]);
                Resources.maxComputeWorkGroupSizeY = static_cast<int>(maxWorkGroupSize[1]);
                Resources.maxComputeWorkGroupSizeZ = static_cast<int>(maxWorkGroupSize[2]);
                Resources.maxComputeWorkGroupCountX = static_cast<int>(maxWorkGroupCount[0]);
                Resources.maxComputeWorkGroupCountY = static_cast<int>(maxWorkGroupCount[1]);
                Resources.maxComputeWorkGroupCountZ = static_cast<int>(maxWorkGroupCount[2]);

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

            namespace {
                // glslang reflects an array-of-arrays default-block uniform as ONE RECORD PER
                // outer-index tuple, carrying the innermost array type: `float u[2][3]` becomes
                // "u[0][0]" and "u[1][0]" (that last "[0]" is EShReflectionBasicArraySuffix). The
                // linker resolves such a name by stripping the single trailing "[0]", so it looks
                // up "u[1]" - a key the root entry alone cannot answer, and the whole declaration
                // silently loses its explicit location.
                //
                // Emit those pre-flattened keys next to the root, so the result is
                // order-independent: each carries the location its own element starts at (element
                // i of `float u[2][3]` at location L starts at L + i*3). Identifiers cannot
                // contain brackets, so a synthesized key never collides with a real uniform name,
                // and a 1-D array needs none of this - stripping "[0]" already reaches the root.
                void RecordArrayOfArraysElementLocations(const String& name, const std::vector<int>& dimensions,
                                                         const long long baseLocation,
                                                         UnorderedMap<String, Int>& locations) {
                    if (dimensions.size() < 2) return;
                    // A pathological declaration must not be able to blow up the map; past the cap
                    // only the root entry stands, which is what every case used to get.
                    constexpr long long kMaxSynthesizedKeys = 4096;
                    const long long innerSpan = dimensions.back();
                    const SizeT outerDimensions = dimensions.size() - 1;
                    long long elementCount = 1;
                    for (SizeT d = 0; d < outerDimensions; ++d) {
                        elementCount *= dimensions[d];
                        if (elementCount > kMaxSynthesizedKeys) return;
                    }
                    for (long long element = 0; element < elementCount; ++element) {
                        String key = name;
                        long long remainder = element;
                        for (SizeT d = 0; d < outerDimensions; ++d) {
                            long long stride = 1;
                            for (SizeT inner = d + 1; inner < outerDimensions; ++inner) stride *= dimensions[inner];
                            key += "[" + std::to_string(remainder / stride) + "]";
                            remainder %= stride;
                        }
                        locations.emplace(key, static_cast<Int>(std::min(baseLocation + element * innerSpan,
                                                                         static_cast<long long>(INT_MAX / 2))));
                    }
                }
            } // namespace

            UnorderedMap<String, Int> CollectExplicitUniformLocations(const glslang::TShader& shader) {
                UnorderedMap<String, Int> locations;
                const glslang::TIntermediate* intermediate = shader.getIntermediate();
                if (intermediate == nullptr) return locations;

                // Half one: the uniforms the relaxed remap swallowed, out of the snapshot it
                // takes on the way past.
                for (const glslang::TIntermediate::TUniformLocation& record :
                     intermediate->getUniformLocations()) {
                    if (record.location < 0) continue;
                    // Keep the first sighting. Two records for one name mean the parser saw the
                    // declaration twice, and the first is the one the symbol table kept.
                    locations.emplace(record.name, record.location);
                    RecordArrayOfArraysElementLocations(record.name, record.arraySizes, record.location,
                                                        locations);
                }

                // Half two: the OPAQUE uniforms, which the remap never touches (the guard in
                // vkRelaxedRemapUniformVariable admits only types containing something
                // non-opaque, atomic_uint, or a sampler inside a struct) and which therefore
                // still carry their qualifier here.
                //
                // They belong in the same map even though reflection could also answer for them,
                // and the distinction is not cosmetic: this map is what marks a location as
                // SOURCE-EXPLICIT, i.e. API contract under ARB_explicit_uniform_location. A
                // location that only reaches DoReflection through glslang's own layoutLocation()
                // is treated as implementation-chosen and quietly moved on a collision, which is
                // the wrong answer for one the shader declared.
                //
                // Read BEFORE any link: mapIO writes its own choices into these same qualifiers
                // (iomapper.cpp:240), so this is only truthful while the shader is unlinked -
                // which is exactly where ShaderCompileTask calls it.
                const glslang::TIntermAggregate* linkerObjects = intermediate->findLinkerObjects();
                if (linkerObjects == nullptr) return locations;
                for (TIntermNode* node : linkerObjects->getSequence()) {
                    const glslang::TIntermSymbol* symbol = node ? node->getAsSymbolNode() : nullptr;
                    if (symbol == nullptr) continue;
                    const glslang::TType& type = symbol->getType();
                    const glslang::TQualifier& qualifier = type.getQualifier();
                    if (qualifier.storage != glslang::EvqUniform || !qualifier.hasLocation()) continue;
                    // A BLOCK has no glGetUniformLocation of its own, and its members are
                    // addressed through the block. Only loose uniforms take locations.
                    if (type.getBasicType() == glslang::EbtBlock || type.isBuiltIn()) continue;

                    std::vector<int> arraySizes;
                    if (type.isArray() && type.getArraySizes() != nullptr) {
                        const glslang::TArraySizes& sizes = *type.getArraySizes();
                        for (int dim = 0; dim < sizes.getNumDims(); ++dim) {
                            arraySizes.push_back(sizes.getDimSize(dim));
                        }
                    }
                    const String name = symbol->getAccessName().c_str();
                    const Int location = static_cast<Int>(qualifier.layoutLocation);
                    locations.emplace(name, location);
                    RecordArrayOfArraysElementLocations(name, arraySizes, location, locations);
                }
                return locations;
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
                                                       attrib.explicitOpaqueUniformBindings,
                                                       attrib.storageBlocksWithoutBinding);
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

            // Total validation failures observed this process. This latch - not the wrappers'
            // return values - is the test-lane signal: validation must never change what a
            // wrapper returns, or the validating lanes would render differently from the
            // shipping configuration (fail-open call sites would silently substitute an
            // earlier-stage module).
            static std::atomic<Uint64> g_spirvValidationFailures{0};

            namespace {
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
                                // Unlatched: only reachable with the validation switch armed,
                                // and every VUID names a different defect. (Parked at MGLOG_I
                                // until the Log.h ordering fix made E live at INFO.)
                                MGLOG_E("[spirv] %s: %s (word index %zu)", site, text, position.index);
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
                void ValidateOrLatch(const char* site, const Vector<Uint32>& binary,
                                     const bool enableSpirvValidation) {
                    if (!enableSpirvValidation) return;
                    PinValidatorTablesForProcessExit();
                    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_1);
                    tools.SetMessageConsumer(MakeSpirvMessageConsumer(site));
                    if (!tools.Validate(binary)) {
                        MGLOG_E("[spirv] %s: produced a module that fails validation (failure #%llu)",
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
                                         Vector<uint32_t>& outputBinary, const bool validateOutput,
                                         const bool enableSpirvValidation) {
                    spvtools::OptimizerOptions options;
                    options.set_run_validator(false);
                    optimizer.SetMessageConsumer(MakeSpirvMessageConsumer(site));
                    if (!optimizer.Run(inputBinary.data(), inputBinary.size(), &outputBinary, options)) {
                        return false;
                    }
                    if (validateOutput) {
                        ValidateOrLatch(site, outputBinary, enableSpirvValidation);
                    }
                    return true;
                }
            } // namespace

            void ShaderCompiler::PrepareSpirvValidation() {
                PinValidatorTablesForProcessExit();
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

            Bool ShaderCompiler::ModuleDeclaresFloat64(const Vector<Uint32>& spirv) {
                if (spirv.empty()) {
                    // Same reasoning as ModuleDeclaresBufferTextureSampler: a stage that produced
                    // no SPIR-V is not a verdict about 64-bit floats, and parsing it would push a
                    // spurious diagnostic through the message consumer.
                    return false;
                }
                std::unique_ptr<spvtools::opt::IRContext> context = spvtools::BuildModule(
                    SPV_ENV_VULKAN_1_1, MakeSpirvMessageConsumer("ModuleDeclaresFloat64"), spirv.data(),
                    spirv.size());
                if (!context) {
                    return false;
                }
                for (const spvtools::opt::Instruction& type : context->types_values()) {
                    if (type.opcode() == spv::Op::OpTypeFloat && type.NumInOperands() >= 1 &&
                        type.GetSingleWordInOperand(0) == 64) {
                        return true;
                    }
                }
                return false;
            }

            namespace {
                // The leaf-width test behind ModuleDeclaresFloat64VertexInput, and it is a LEAF
                // test rather than a shape test on purpose: a `dmat4` input is an OpTypeMatrix of
                // OpTypeVector of OpTypeFloat 64, and it is as unfetchable as a bare `double`.
                Bool TypeHoldsFloat64(const spvtools::opt::analysis::Type* type) {
                    if (type == nullptr) return false;
                    if (const auto* scalar = type->AsFloat()) return scalar->width() == 64;
                    if (const auto* vector = type->AsVector()) return TypeHoldsFloat64(vector->element_type());
                    if (const auto* matrix = type->AsMatrix()) return TypeHoldsFloat64(matrix->element_type());
                    if (const auto* array = type->AsArray()) return TypeHoldsFloat64(array->element_type());
                    return false;
                }
            } // namespace

            Bool ShaderCompiler::ModuleDeclaresFloat64VertexInput(const Vector<Uint32>& spirv) {
                if (spirv.empty()) {
                    return false;
                }
                std::unique_ptr<spvtools::opt::IRContext> context = spvtools::BuildModule(
                    SPV_ENV_VULKAN_1_1, MakeSpirvMessageConsumer("ModuleDeclaresFloat64VertexInput"),
                    spirv.data(), spirv.size());
                if (!context) {
                    return false;
                }
                // Vertex only. Every other stage's inputs come from another stage's outputs, which
                // MobileGL never re-formats, so a 64-bit varying between two stages is the driver's
                // business and not this question's.
                auto entryPoints = context->module()->entry_points();
                if (entryPoints.begin() == entryPoints.end()) return false;
                const spvtools::opt::Instruction& entryPoint = *entryPoints.begin();
                if (static_cast<spv::ExecutionModel>(entryPoint.GetSingleWordInOperand(0)) !=
                    spv::ExecutionModel::Vertex) {
                    return false;
                }
                auto* typeManager = context->get_type_mgr();
                auto* defUseManager = context->get_def_use_mgr();
                for (const spvtools::opt::Instruction& variable : context->module()->types_values()) {
                    if (variable.opcode() != spv::Op::OpVariable || variable.NumInOperands() < 1) continue;
                    if (static_cast<spv::StorageClass>(variable.GetSingleWordInOperand(0)) !=
                        spv::StorageClass::Input) {
                        continue;
                    }
                    const spvtools::opt::Instruction* pointerType = defUseManager->GetDef(variable.type_id());
                    if (pointerType == nullptr || pointerType->NumInOperands() < 2) continue;
                    if (TypeHoldsFloat64(typeManager->GetType(pointerType->GetSingleWordInOperand(1)))) {
                        return true;
                    }
                }
                return false;
            }

            Bool ShaderCompiler::ModuleReadsLocatedInput(const Vector<Uint32>& spirv) {
                if (spirv.empty()) {
                    return false;
                }
                std::unique_ptr<spvtools::opt::IRContext> context = spvtools::BuildModule(
                    SPV_ENV_VULKAN_1_1, MakeSpirvMessageConsumer("ModuleReadsLocatedInput"), spirv.data(),
                    spirv.size());
                if (!context) {
                    return false;
                }
                // A LOCATION is exactly the property that separates a user-defined varying (or a
                // per-patch input) from a built-in: gl_in, gl_TessCoord, gl_PatchVerticesIn,
                // gl_PrimitiveID and the tessellation levels carry none, and every one of them is
                // either forwarded by the pass-through or generated by the tessellator itself.
                //
                // Decided on the OpVariable's own Location decoration rather than on any
                // built-in classification, for the reason DirectVulkan's
                // ReflectPassthroughTessControlNeed records at length: gl_in is an ARRAY OF
                // INTERFACE BLOCKS, and a member walk of one reads back as BuiltIn::Position for
                // every member, so classifying by built-in would accept anything.
                for (auto& variable : context->module()->types_values()) {
                    if (variable.opcode() != spv::Op::OpVariable || variable.NumInOperands() < 1) {
                        continue;
                    }
                    if (static_cast<spv::StorageClass>(variable.GetSingleWordInOperand(0)) !=
                        spv::StorageClass::Input) {
                        continue;
                    }
                    Bool located = false;
                    context->get_decoration_mgr()->ForEachDecoration(
                        variable.result_id(), static_cast<uint32_t>(spv::Decoration::Location),
                        [&located](const spvtools::opt::Instruction&) { located = true; });
                    if (located) return true;
                }
                return false;
            }

            bool ShaderCompiler::DemoteFloat64ToFloat32(const Vector<Uint32>& inputBinary,
                                                        Vector<uint32_t>& outputBinary,
                                                        const bool enableSpirvValidation) {
                using namespace spvtools;
                Optimizer optimizer(SPV_ENV_VULKAN_1_1);
                optimizer.RegisterPass(DemoteFloat64Pass::CreateDemoteFloat64Pass());

                return RunOptimizerChecked("DemoteFloat64ToFloat32", optimizer, inputBinary, outputBinary, true, enableSpirvValidation);
            }

            bool ShaderCompiler::SanitizeAndOptimizeBinary(const Vector<Uint32>& inputBinary,
                                                           Vector<uint32_t>& outputBinary,
                                                           const bool validateOutput,
                                                           const bool enableSpirvValidation,
                                                           const bool nativeFloat64) {
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
                // The fp64 tail, and the ONE part of this chain that is not the same on every
                // backend. Both passes are skipped when the backend can consume Float64 itself
                // (`nativeFloat64`, i.e. VkPhysicalDeviceFeatures::shaderFloat64 on DirectVulkan):
                // there is nothing to emulate then, and narrowing would only throw away precision
                // the driver was willing to give. That is DirectVulkan-on-lavapipe today and
                // nothing else - Adreno and Mali both report shaderFloat64 == VK_FALSE, and
                // DirectGLES can never qualify because GLSL ES has no fp64 type for SPIRV-Cross to
                // emit at all, so on every real mobile device this branch is not taken and the two
                // passes run exactly as they always have.
                //
                // Demoting here - in the one chain every module goes through, at link - is what
                // makes `double` compile at all where the hardware has none, and makes it behave
                // the SAME across both backends of such a device, which matters because the GL
                // frontend's uniform storage is per PROGRAM rather than per call: the glUniform*d
                // shadow narrows to float to match this. Runs last so no earlier pass ever has to
                // reason about a width it will not see in the output; in particular it runs before
                // the backends' PackDoubleVertexInputsPass, whose OpBitcast this one would
                // otherwise decline on. Costs one types_values() walk on the overwhelming majority
                // of modules, which declare no 64-bit float at all.
                // ...but demoting a double that lives in a SHADER STORAGE BLOCK also repacks that
                // block, and the bytes an application put in the buffer do not move with it. This
                // runs first and takes those blocks out of the demotion's hands: each becomes a
                // flat `uint` array whose index arithmetic carries the std140/std430 offsets
                // glslang computed WITH the doubles in place, so the layout survives byte for byte
                // and only the VALUES narrow. Gated on a block actually holding a 64-bit float, so
                // every other module pays one types_values() walk and nothing else, and it declines
                // (leaving the block for the demotion to handle the old way) on any shape it cannot
                // re-address exactly. See FlattenFloat64StorageBlockPass.h. It is skipped with the
                // demotion rather than kept: its whole purpose is to preserve the byte layout ACROSS
                // a narrowing that is no longer happening, and flattening a block a native driver
                // would have laid out correctly by itself only costs the shader its index
                // arithmetic.
                if (!nativeFloat64) {
                    optimizer.RegisterPass(
                        FlattenFloat64StorageBlockPass::CreateFlattenFloat64StorageBlockPass());
                    optimizer.RegisterPass(DemoteFloat64Pass::CreateDemoteFloat64Pass());
                }

                return RunOptimizerChecked("SanitizeAndOptimizeBinary", optimizer, inputBinary,
                                           outputBinary, validateOutput, enableSpirvValidation);
            }

            bool ShaderCompiler::LowerDrawParametersForEssl(const Vector<Uint32>& inputBinary,
                                                            Vector<uint32_t>& outputBinary,
                                                            const bool enableSpirvValidation) {
                using namespace spvtools;
                Optimizer optimizer(SPV_ENV_VULKAN_1_1);
                optimizer.RegisterPass(LowerDrawParametersPass::CreateLowerDrawParametersPass());

                return RunOptimizerChecked("LowerDrawParametersForEssl", optimizer, inputBinary,
                                           outputBinary, true, enableSpirvValidation);
            }

            bool ShaderCompiler::LowerViewportIndexForEssl(const Vector<Uint32>& inputBinary,
                                                           Vector<uint32_t>& outputBinary,
                                                           const bool enableSpirvValidation) {
                using namespace spvtools;
                Optimizer optimizer(SPV_ENV_VULKAN_1_1);
                optimizer.RegisterPass(LowerViewportIndexPass::CreateLowerViewportIndexPass());

                return RunOptimizerChecked("LowerViewportIndexForEssl", optimizer, inputBinary,
                                           outputBinary, true, enableSpirvValidation);
            }

            bool ShaderCompiler::DeclaresViewportIndexBuiltin(const Vector<Uint32>& binary) {
                return LowerViewportIndexPass::DeclaresViewportIndexBuiltin(binary);
            }

            bool ShaderCompiler::ClampMultisampleFetchesForEssl(const Vector<Uint32>& inputBinary,
                                                                Vector<uint32_t>& outputBinary,
                                                                const Int32 maxColorSamples,
                                                                const Int32 maxIntegerSamples,
                                                                const Int32 maxDepthSamples,
                                                                const Int32 advertisedMaxSamples,
                                                                const bool enableSpirvValidation) {
                using namespace spvtools;
                Optimizer optimizer(SPV_ENV_VULKAN_1_1);
                optimizer.RegisterPass(ClampMultisampleFetchPass::CreateClampMultisampleFetchPass(
                    maxColorSamples, maxIntegerSamples, maxDepthSamples, advertisedMaxSamples));

                return RunOptimizerChecked("ClampMultisampleFetchesForEssl", optimizer, inputBinary,
                                           outputBinary, true, enableSpirvValidation);
            }

            bool ShaderCompiler::DeclaresMultisampledImage(const Vector<Uint32>& binary) {
                return ClampMultisampleFetchPass::DeclaresMultisampledImage(binary);
            }

            ShaderCompiler::SpirvGateFeatures ShaderCompiler::ProbeSpirvGateFeatures(
                const Vector<Uint32>& binary) {
                SpirvGateFeatures features;
                if (binary.empty()) {
                    return features;
                }
                std::unique_ptr<spvtools::opt::IRContext> context = spvtools::BuildModule(
                    SPV_ENV_VULKAN_1_1,
                    [](spv_message_level_t, const char*, const spv_position_t&, const char*) {},
                    binary.data(), binary.size());
                if (!context) {
                    // Unparseable here means unusable downstream too; let the ordinary transpile
                    // path produce the error rather than inventing a verdict from it.
                    return features;
                }
                features.WritesViewportIndexOutput =
                    LowerViewportIndexPass::DeclaresViewportIndexBuiltin(context.get());
                features.DeclaresMultisampledImage =
                    ClampMultisampleFetchPass::DeclaresMultisampledImage(context.get());
                return features;
            }

            bool ShaderCompiler::SplitArrayVertexInputsForEssl(const Vector<Uint32>& inputBinary,
                                                               Vector<uint32_t>& outputBinary,
                                                               const bool enableSpirvValidation) {
                using namespace spvtools;
                Optimizer optimizer(SPV_ENV_VULKAN_1_1);
                optimizer.RegisterPass(SplitArrayVertexInputsPass::CreateSplitArrayVertexInputsPass());

                return RunOptimizerChecked("SplitArrayVertexInputsForEssl", optimizer, inputBinary,
                                           outputBinary, true, enableSpirvValidation);
            }

            bool ShaderCompiler::BakeImageFormatsForEssl(const Vector<Uint32>& inputBinary,
                                                         const UnorderedMap<String, Uint>& glFormatByName,
                                                         Vector<uint32_t>& outputBinary,
                                                         const bool enableSpirvValidation) {
                using namespace spvtools;
                if (glFormatByName.empty()) return false;
                Optimizer optimizer(SPV_ENV_VULKAN_1_1);
                optimizer.RegisterPass(BakeImageFormatsPass::CreateBakeImageFormatsPass(glFormatByName));

                return RunOptimizerChecked("BakeImageFormatsForEssl", optimizer, inputBinary, outputBinary, true, enableSpirvValidation);
            }

            bool ShaderCompiler::DeclaresFormatlessStorageImage(const Vector<Uint32>& binary) {
                return BakeImageFormatsPass::DeclaresFormatlessStorageImage(binary);
            }

            bool ShaderCompiler::GLInternalFormatIsCoreEsslImageFormat(Uint glInternalFormat) {
                return BakeImageFormatsPass::IsCoreEsslImageFormat(
                    BakeImageFormatsPass::SpirvImageFormatFromGLInternalFormat(glInternalFormat));
            }

            String ShaderCompiler::EsslImageFormatSpelling(Uint glInternalFormat) {
                return BakeImageFormatsPass::EsslSpellingOfGLInternalFormat(glInternalFormat);
            }

            bool ShaderCompiler::SpirvCrossCanPrintEsslImageFormat(Uint glInternalFormat) {
                return BakeImageFormatsPass::IsSpirvCrossEsslPrintableFormat(
                    BakeImageFormatsPass::SpirvImageFormatFromGLInternalFormat(glInternalFormat));
            }

            bool ShaderCompiler::WidenImageFormatsForEssl(const Vector<Uint32>& inputBinary,
                                                          Vector<uint32_t>& outputBinary,
                                                          const bool onlyFormatsSpirvCrossRefusesToPrint,
                                                          const bool enableSpirvValidation) {
                using namespace spvtools;
                Optimizer optimizer(SPV_ENV_VULKAN_1_1);
                optimizer.RegisterPass(
                    WidenImageFormatsPass::CreateWidenImageFormatsPass(onlyFormatsSpirvCrossRefusesToPrint));
                // Two image types that differed only in a format the widening collapses -
                // `layout(rg32f)` and `layout(rgba32f)` in one module - are one type afterwards,
                // and duplicate non-aggregate type declarations are invalid SPIR-V. This joins
                // them, and cascades to the pointer and array types that named them; the pass
                // itself deliberately does not carry a join of its own.
                optimizer.RegisterPass(CreateRemoveDuplicatesPass());

                return RunOptimizerChecked("WidenImageFormatsForEssl", optimizer, inputBinary, outputBinary,
                                           true, enableSpirvValidation);
            }

            bool ShaderCompiler::DeclaresWidenableImageFormat(const Vector<Uint32>& binary,
                                                              const bool onlyFormatsSpirvCrossRefusesToPrint) {
                return WidenImageFormatsPass::DeclaresWidenableImageFormat(binary,
                                                                           onlyFormatsSpirvCrossRefusesToPrint);
            }

            Uint ShaderCompiler::WidenedCoreEsslImageFormat(Uint glInternalFormat) {
                return WidenImageFormatsPass::WidenedCoreEsslImageFormat(glInternalFormat);
            }

            Uint ShaderCompiler::ImageFormatChannelCount(Uint glInternalFormat) {
                return WidenImageFormatsPass::ImageFormatChannelCount(glInternalFormat);
            }

            bool ShaderCompiler::NormalizedImageCarrierCodes(Uint glInternalFormat, Uint32 (&outChannelMax)[4],
                                                             bool& outSignedNormalized) {
                return WidenImageFormatsPass::NormalizedImageCarrierCodes(glInternalFormat, outChannelMax,
                                                                          outSignedNormalized);
            }

            bool ShaderCompiler::FlattenXfbInterfaceBlocksForEssl(const Vector<Uint32>& inputBinary,
                                                                  const std::set<String>& blockNames,
                                                                  std::set<String>& flattenedBlockNames,
                                                                  Vector<uint32_t>& outputBinary,
                                                                  const bool enableSpirvValidation) {
                using namespace spvtools;
                if (blockNames.empty()) return false;
                Optimizer optimizer(SPV_ENV_VULKAN_1_1);
                optimizer.RegisterPass(FlattenXfbInterfaceBlocksPass::CreateFlattenXfbInterfaceBlocksPass(
                    blockNames, &flattenedBlockNames));

                return RunOptimizerChecked("FlattenXfbInterfaceBlocksForEssl", optimizer, inputBinary,
                                           outputBinary, true, enableSpirvValidation);
            }

            bool ShaderCompiler::RewriteXfbCaptureNameForFlattenedBlock(
                const String& captureName, const std::set<String>& flattenedBlockNames, String& outName) {
                return FlattenXfbInterfaceBlocksPass::RewriteCaptureName(captureName, flattenedBlockNames,
                                                                         outName);
            }

            void ShaderCompiler::ProbeIoBlockNamesForEssl(const Vector<Uint32>& binary,
                                                          std::set<String>& collidingBlockNames,
                                                          std::set<String>& declaredNames) {
                if (binary.empty()) {
                    // Same reasoning as ModuleDeclaresBufferTextureSampler: a stage that produced
                    // no SPIR-V has no block names to report, and parsing it would push a
                    // spurious diagnostic through the message consumer.
                    return;
                }
                std::unique_ptr<spvtools::opt::IRContext> context = spvtools::BuildModule(
                    SPV_ENV_VULKAN_1_1, MakeSpirvMessageConsumer("ProbeIoBlockNamesForEssl"), binary.data(),
                    binary.size());
                if (!context) {
                    // Unparseable here means unusable downstream too; let the ordinary transpile
                    // path produce the error rather than inventing a rename plan from it.
                    return;
                }
                UniquifyIoBlockNamesPass::ProbeIoBlockNames(context.get(), collidingBlockNames, declaredNames);
            }

            bool ShaderCompiler::UniquifyIoBlockNamesForEssl(const Vector<Uint32>& inputBinary,
                                                             const std::map<String, String>& inputBlockRenames,
                                                             const std::map<String, String>& outputBlockRenames,
                                                             std::set<String>& renamedBlockNames,
                                                             Vector<uint32_t>& outputBinary,
                                                             const bool enableSpirvValidation) {
                using namespace spvtools;
                if (inputBlockRenames.empty() && outputBlockRenames.empty()) return false;
                Optimizer optimizer(SPV_ENV_VULKAN_1_1);
                optimizer.RegisterPass(UniquifyIoBlockNamesPass::CreateUniquifyIoBlockNamesPass(
                    inputBlockRenames, outputBlockRenames, &renamedBlockNames));

                return RunOptimizerChecked("UniquifyIoBlockNamesForEssl", optimizer, inputBinary,
                                           outputBinary, true, enableSpirvValidation);
            }

            bool ShaderCompiler::PackDoubleVertexInputsForVulkan(const Vector<Uint32>& inputBinary,
                                                                 Vector<uint32_t>& outputBinary,
                                                                 const bool enableSpirvValidation) {
                using namespace spvtools;
                Optimizer optimizer(SPV_ENV_VULKAN_1_1);
                optimizer.RegisterPass(PackDoubleVertexInputsPass::CreatePackDoubleVertexInputsPass());

                return RunOptimizerChecked("PackDoubleVertexInputsForVulkan", optimizer, inputBinary,
                                           outputBinary, true, enableSpirvValidation);
            }

            bool ShaderCompiler::StripUboMemberRelaxedPrecisionForEssl(const Vector<Uint32>& inputBinary,
                                                                       Vector<uint32_t>& outputBinary,
                                                                       const bool enableSpirvValidation) {
                using namespace spvtools;
                Optimizer optimizer(SPV_ENV_VULKAN_1_1);
                optimizer.RegisterPass(
                    StripUboMemberRelaxedPrecisionPass::CreateStripUboMemberRelaxedPrecisionPass());

                return RunOptimizerChecked("StripUboMemberRelaxedPrecisionForEssl", optimizer,
                                           inputBinary, outputBinary, true, enableSpirvValidation);
            }

            bool ShaderCompiler::StripNoPerspectiveForEssl(const Vector<Uint32>& inputBinary,
                                                           Vector<uint32_t>& outputBinary,
                                                           const bool enableSpirvValidation) {
                using namespace spvtools;
                Optimizer optimizer(SPV_ENV_VULKAN_1_1);
                optimizer.RegisterPass(StripNoPerspectivePass::CreateStripNoPerspectivePass());

                return RunOptimizerChecked("StripNoPerspectiveForEssl", optimizer, inputBinary,
                                           outputBinary, true, enableSpirvValidation);
            }

            bool ShaderCompiler::EmulateNoPerspectiveForEssl(const Vector<Uint32>& inputBinary,
                                                             Vector<uint32_t>& outputBinary,
                                                             const bool enableSpirvValidation) {
                using namespace spvtools;
                Optimizer optimizer(SPV_ENV_VULKAN_1_1);
                optimizer.RegisterPass(EmulateNoPerspectivePass::CreateEmulateNoPerspectivePass());

                return RunOptimizerChecked("EmulateNoPerspectiveForEssl", optimizer, inputBinary,
                                           outputBinary, true, enableSpirvValidation);
            }

            bool ShaderCompiler::LegalizeFragmentOutputIndexingForEssl(const Vector<Uint32>& inputBinary,
                                                                       Vector<uint32_t>& outputBinary,
                                                                       const bool enableSpirvValidation) {
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
                                         folded, true, enableSpirvValidation) ||
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
                                         outputBinary, true, enableSpirvValidation) ||
                    outputBinary.empty()) {
                    outputBinary = folded;
                    return true;
                }

                if (LegalizeFragmentOutputIndexPass::BinaryHasDynamicOutputIndexing(outputBinary)) {
                    // MGLOG_W, latched: this runs per shader compile, and shader packs compile
                    // lazily mid-session, so an unlatched line here is unbounded runtime noise.
                    // (Parked at MGLOG_I until the Log.h ordering fix made W live at INFO.)
                    MGLOG_W_ONCE("[spirv] LegalizeFragmentOutputIndexingForEssl: a fragment output is still "
                            "indexed dynamically; a strict ES driver will reject this shader");
                }
                return true;
            }

            bool ShaderCompiler::LegalizeResourceArrayIndexingForEssl(
                const Vector<Uint32>& inputBinary, Vector<uint32_t>& outputBinary,
                const bool enableSpirvValidation) {
                using namespace spvtools;

                // Detection gates everything: a module that declares no array of storage blocks
                // and no array of images, or indexes one only with constants - every shader but
                // a handful - pays one BuildModule and is handed back byte for byte, so the
                // folding chain can never perturb a shader that did not need it.
                if (!LegalizeResourceArrayIndexPass::BinaryHasDynamicResourceArrayIndexing(
                        inputBinary)) {
                    outputBinary = inputBinary;
                    return true;
                }

                // Stock passes do the real work, exactly as in the fragment-output
                // legalization. The only bespoke member of the chain is the loop-control hint
                // the stock unroller demands (see the pass header); with it set, the
                // `for (i = 0; i < 4; ++i) arr[i]...` shape folds to literals here and the
                // fallback below never runs.
                Optimizer folder(SPV_ENV_VULKAN_1_1);
                // First, because both the unroller and the marking pass below read the
                // induction variable as an OpPhi, and glslang emits it as loads and stores of
                // a Function variable.
                folder.RegisterPass(CreateLocalMultiStoreElimPass());
                folder.RegisterPass(LegalizeResourceArrayIndexPass::CreateMarkLoopsForUnrollPass());
                folder.RegisterPass(CreateLoopUnrollPass(true));
                // Fold the unrolled induction values into the access chains, then clear out
                // what constant conditions leave behind.
                folder.RegisterPass(CreateCCPPass());
                folder.RegisterPass(CreateSimplificationPass());
                folder.RegisterPass(CreateDeadBranchElimPass());
                folder.RegisterPass(CreateBlockMergePass());

                Vector<uint32_t> folded;
                if (!RunOptimizerChecked("LegalizeResourceArrayIndexingForEssl.fold", folder,
                                         inputBinary, folded, true, enableSpirvValidation) ||
                    folded.empty()) {
                    // Fail open onto the fallback rather than onto the illegal module.
                    folded = inputBinary;
                }

                if (!LegalizeResourceArrayIndexPass::BinaryHasDynamicResourceArrayIndexing(
                        folded)) {
                    outputBinary = folded;
                    return true;
                }

                // Genuinely dynamic (uniform-derived, non-constant trip count, ...): lower it.
                Optimizer lowerer(SPV_ENV_VULKAN_1_1);
                lowerer.RegisterPass(
                    LegalizeResourceArrayIndexPass::CreateLowerToConstantSwitchPass());
                // The chains the lowering replaced are dead now; remove_outputs must stay
                // false here for the same reason it does in SanitizeAndOptimizeBinary.
                lowerer.RegisterPass(CreateAggressiveDCEPass(false));

                if (!RunOptimizerChecked("LegalizeResourceArrayIndexingForEssl.lower", lowerer, folded,
                                         outputBinary, true, enableSpirvValidation) ||
                    outputBinary.empty()) {
                    outputBinary = folded;
                    return true;
                }

                if (LegalizeResourceArrayIndexPass::BinaryHasDynamicResourceArrayIndexing(
                        outputBinary)) {
                    // MGLOG_W, latched, for the same reason the fragment-output one is: this
                    // runs per shader compile and shader packs compile lazily mid-session.
                    MGLOG_W_ONCE("[spirv] LegalizeResourceArrayIndexingForEssl: an array of storage "
                                 "blocks or of images is still indexed dynamically; a strict ES "
                                 "driver will reject this shader");
                }
                return true;
            }

            bool ShaderCompiler::FlattenAtomicCounterBlockOffsetsForEssl(
                const Vector<Uint32>& inputBinary, Vector<uint32_t>& outputBinary,
                const bool enableSpirvValidation) {
                using namespace spvtools;

                // Detection gates everything: a module with no atomic counter, or one whose
                // counters sit at their natural std430 offsets - which is every shader that omits
                // the offset qualifier - pays one BuildModule and is handed back byte for byte.
                if (!FlattenAtomicCounterBlockPass::BinaryHasOffsetAtomicCounterBlock(inputBinary)) {
                    outputBinary = inputBinary;
                    return true;
                }

                Optimizer optimizer(SPV_ENV_VULKAN_1_1);
                optimizer.RegisterPass(FlattenAtomicCounterBlockPass::CreateFlattenAtomicCounterBlockPass());

                return RunOptimizerChecked("FlattenAtomicCounterBlockOffsetsForEssl", optimizer, inputBinary,
                                           outputBinary, true, enableSpirvValidation);
            }

            bool ShaderCompiler::LowerRectImages(const Vector<Uint32>& inputBinary,
                                                 Vector<uint32_t>& outputBinary,
                                                 const bool enableSpirvValidation) {
                using namespace spvtools;
                Optimizer optimizer(SPV_ENV_VULKAN_1_1);
                optimizer.RegisterPass(NormalizeRectCoordinatesPass::CreateNormalizeRectCoordinatesPass());

                return RunOptimizerChecked("LowerRectImages", optimizer, inputBinary, outputBinary, true, enableSpirvValidation);
            }

            bool ShaderCompiler::Lower1DArrayImagesForEssl(const Vector<Uint32>& inputBinary,
                                                            Vector<uint32_t>& outputBinary, const bool enableSpirvValidation) {
                using namespace spvtools;

                // Declined rather than half-translated: after the rewrite the image is a 2D
                // (array) one, so a size query on it yields a component more than the shader
                // consumes. Handing back a differently-shaped size silently is worse than leaving
                // the module alone and letting the driver say what it does not like - and unlike
                // the access path there is no correct answer to substitute, because the ES texture
                // genuinely has a height the GL one does not.
                //
                // MGLOG_W, latched: per shader compile, and shader packs compile lazily
                // mid-session. (Parked at MGLOG_I until the Log.h ordering fix made W live.)
                const auto traits = Lower1DArrayImagesPass::InspectBinary(inputBinary);
                // The overwhelmingly common answer, and the reason the inspection exists: no
                // 1D storage image this pass owns, so the module is handed back byte for byte
                // without an Optimizer ever being built. Every ESSL shader in the process passes
                // through here, so the cost of the case with nothing to do is the cost of this
                // pass.
                if (!traits.declaresImage) {
                    outputBinary = inputBinary;
                    return true;
                }
                if (traits.queriesImageSize) {
                    MGLOG_W_ONCE("[spirv] Lower1DArrayImagesForEssl: the module queries the size of a 1D "
                            "storage image, which cannot be answered in the 2D shape ES stores it in; "
                            "leaving the module alone, and a strict ES driver will reject it");
                    outputBinary = inputBinary;
                    return true;
                }

                Optimizer optimizer(SPV_ENV_VULKAN_1_1);
                optimizer.RegisterPass(Lower1DArrayImagesPass::CreateLower1DArrayImagesPass());
                // Mandatory, not tidying. Rewriting a 1D(-array) image type to the 2D(-array) one
                // makes it structurally IDENTICAL to any real 2D(-array) image of the same sampled
                // type and format that the module already declared - and SPIR-V forbids duplicate
                // non-aggregate type declarations, so the result fails validation. That collision
                // is not exotic: it is the shape of this whole change's headline case, where one
                // compute shader declares uimage1DArray and uimage2DArray side by side, both
                // r32ui. The same applies one level up, to the OpTypePointer instructions that
                // named the two types, and to the Image1D capability the rewrite turns into a
                // second Shader. Deduplicating afterwards collapses all three at once.
                optimizer.RegisterPass(CreateRemoveDuplicatesPass());

                return RunOptimizerChecked("Lower1DArrayImagesForEssl", optimizer, inputBinary, outputBinary, true, enableSpirvValidation);
            }

            bool ShaderCompiler::Lower1DSampledImagesForEssl(const Vector<Uint32>& inputBinary,
                                                             Vector<uint32_t>& outputBinary,
                                                             const bool enableSpirvValidation) {
                using namespace spvtools;

                // The overwhelmingly common answer, and the reason the probe exists: no 1D sampler
                // is reached by an offset or a gradient, so the module is handed back byte for
                // byte without an Optimizer ever being built. Every ESSL shader in the process
                // passes through here, so the cost of the case with nothing to do is the cost of
                // this pass. Note the probe is deliberately NARROWER than "declares a 1D sampler":
                // SPIRV-Cross emits the plain sample and fetch forms correctly, and taking those
                // over would be a regression looking for somewhere to happen.
                if (!Lower1DSampledImagesPass::BinaryHasOffsetOrGrad1DSampledImage(inputBinary)) {
                    outputBinary = inputBinary;
                    return true;
                }

                Optimizer optimizer(SPV_ENV_VULKAN_1_1);
                optimizer.RegisterPass(Lower1DSampledImagesPass::CreateLower1DSampledImagesPass());
                // Mandatory, not tidying - the same collision Lower1DArrayImagesForEssl documents
                // one screen up. Rewriting a 1D sampled image type to the 2D one makes it
                // structurally IDENTICAL to any real 2D sampled image of the same sampled type the
                // module already declared, and SPIR-V forbids duplicate non-aggregate type
                // declarations. That is not exotic here: it is the exact shape of the headline
                // case, whose compute shader declares sampler1D and sampler2D side by side. The
                // same applies to the OpTypeSampledImage and OpTypePointer instructions above
                // them, and to the Sampled1D capability the rewrite turns into a second Shader.
                optimizer.RegisterPass(CreateRemoveDuplicatesPass());

                return RunOptimizerChecked("Lower1DSampledImagesForEssl", optimizer, inputBinary,
                                           outputBinary, true, enableSpirvValidation);
            }

            bool ShaderCompiler::RebaseInstanceIndexForVulkan(const Vector<Uint32>& inputBinary,
                                                              Vector<uint32_t>& outputBinary, const bool enableSpirvValidation) {
                using namespace spvtools;
                Optimizer optimizer(SPV_ENV_VULKAN_1_1);
                optimizer.RegisterPass(RebaseInstanceIndexPass::CreateRebaseInstanceIndexPass());

                return RunOptimizerChecked("RebaseInstanceIndexForVulkan", optimizer, inputBinary,
                                           outputBinary, true, enableSpirvValidation);
            }

            bool ShaderCompiler::ZeroBaseVertexForVulkan(const Vector<Uint32>& inputBinary,
                                                         Vector<uint32_t>& outputBinary, const bool enableSpirvValidation) {
                using namespace spvtools;
                Optimizer optimizer(SPV_ENV_VULKAN_1_1);
                optimizer.RegisterPass(ZeroBaseVertexPass::CreateZeroBaseVertexPass());

                return RunOptimizerChecked("ZeroBaseVertexForVulkan", optimizer, inputBinary, outputBinary, true, enableSpirvValidation);
            }

            bool ShaderCompiler::DeriveNumSubgroupsForVulkan(const Vector<Uint32>& inputBinary,
                                                             Vector<uint32_t>& outputBinary,
                                                             const bool enableSpirvValidation) {
                using namespace spvtools;
                Optimizer optimizer(SPV_ENV_VULKAN_1_1);
                optimizer.RegisterPass(DeriveNumSubgroupsPass::CreateDeriveNumSubgroupsPass());

                return RunOptimizerChecked("DeriveNumSubgroupsForVulkan", optimizer, inputBinary,
                                           outputBinary, true, enableSpirvValidation);
            }

            bool ShaderCompiler::EmulateSubgroupsForVulkan(const Vector<Uint32>& inputBinary,
                                                           Vector<uint32_t>& outputBinary,
                                                           const Uint32 maxWorkgroupScratchBytes,
                                                           const bool enableSpirvValidation) {
                using namespace spvtools;
                Optimizer optimizer(SPV_ENV_VULKAN_1_1);
                optimizer.RegisterPass(
                    EmulateSubgroupsPass::CreateEmulateSubgroupsPass(maxWorkgroupScratchBytes));

                return RunOptimizerChecked("EmulateSubgroupsForVulkan", optimizer, inputBinary,
                                           outputBinary, true, enableSpirvValidation);
            }

            bool ShaderCompiler::FixIterationRPSubgroupScratchForVulkan(
                const Vector<Uint32>& inputBinary, Vector<uint32_t>& outputBinary,
                const Uint32 nativeSubgroupSize, const Uint32 maxWorkgroupScratchBytes,
                const bool enableSpirvValidation) {
                using namespace spvtools;
                Optimizer optimizer(SPV_ENV_VULKAN_1_1);
                optimizer.RegisterPass(
                    FixIterationRPSubgroupScratchPass::CreateFixIterationRPSubgroupScratchPass(
                        nativeSubgroupSize, maxWorkgroupScratchBytes));

                return RunOptimizerChecked("FixIterationRPSubgroupScratchForVulkan", optimizer,
                                           inputBinary, outputBinary, true, enableSpirvValidation);
            }

            bool ShaderCompiler::FixIterationRPBarrierForVulkan(
                const Vector<Uint32>& inputBinary, Vector<uint32_t>& outputBinary,
                const bool enableSpirvValidation) {
                using namespace spvtools;
                Optimizer optimizer(SPV_ENV_VULKAN_1_1);
                optimizer.RegisterPass(FixIterationRPBarrierPass::CreateFixIterationRPBarrierPass());

                return RunOptimizerChecked("FixIterationRPBarrierForVulkan", optimizer,
                                           inputBinary, outputBinary, true, enableSpirvValidation);
            }

            bool ShaderCompiler::DecoratePositionInvariantForVulkan(const Vector<Uint32>& inputBinary,
                                                                    Vector<uint32_t>& outputBinary, const bool enableSpirvValidation) {
                using namespace spvtools;
                Optimizer optimizer(SPV_ENV_VULKAN_1_1);
                optimizer.RegisterPass(DecoratePositionInvariantPass::CreateDecoratePositionInvariantPass());

                return RunOptimizerChecked("DecoratePositionInvariantForVulkan", optimizer, inputBinary,
                                           outputBinary, true, enableSpirvValidation);
            }

            bool ShaderCompiler::UseUnformattedFloatStorageImagesForVulkan(
                const Vector<Uint32>& inputBinary, Vector<uint32_t>& outputBinary,
                const bool enableSpirvValidation) {
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
                ValidateOrLatch("UseUnformattedFloatStorageImagesForVulkan", outputBinary,
                                enableSpirvValidation);
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
