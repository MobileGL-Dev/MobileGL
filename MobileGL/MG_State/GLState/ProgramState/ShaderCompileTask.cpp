// MobileGL - MobileGL/MG_State/GLState/ProgramState/ShaderCompileTask.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "ShaderCompileTask.h"

#include <MG_State/GLState/BufferState/BufferState.h>
#include <MG_Util/Converters/MGToGL/ProgramEnumConverter.h>
#include <MG_Util/ShaderTranspiler/ShaderCompiler.h>
#include <MG_Util/ShaderTranspiler/ShaderSourceProcessor.h>
#include <MG_Util/ShaderTranspiler/TranslationCache.h>
#include <MG_Util/ShaderTranspiler/Types.h>

#include <glslang/Include/PoolAlloc.h>

#include <algorithm>
#include <charconv>

namespace {
    struct ComputeLocalSize {
        MobileGL::Uint x = 1;
        MobileGL::Uint y = 1;
        MobileGL::Uint z = 1;
        bool declared = false;
    };

    static MobileGL::String StripGlslComments(const MobileGL::String& source) {
        MobileGL::String result;
        result.reserve(source.length());

        bool inLineComment = false;
        bool inBlockComment = false;
        for (MobileGL::SizeT i = 0; i < source.length(); ++i) {
            if (inLineComment) {
                if (source[i] == '\n') {
                    inLineComment = false;
                    result.push_back(source[i]);
                } else {
                    result.push_back(' ');
                }
                continue;
            }

            if (inBlockComment) {
                if (source[i] == '*' && i + 1 < source.length() && source[i + 1] == '/') {
                    inBlockComment = false;
                    result.append("  ");
                    ++i;
                } else {
                    result.push_back(source[i] == '\n' ? '\n' : ' ');
                }
                continue;
            }

            if (source[i] == '/' && i + 1 < source.length()) {
                if (source[i + 1] == '/') {
                    inLineComment = true;
                    result.append("  ");
                    ++i;
                    continue;
                }
                if (source[i + 1] == '*') {
                    inBlockComment = true;
                    result.append("  ");
                    ++i;
                    continue;
                }
            }

            result.push_back(source[i]);
        }

        return result;
    }

    // Hoisted out of ParseComputeLocalSize: constructing a std::regex costs far more than
    // running it over a small source, and it was being rebuilt on every compute compile. A
    // const regex carries no mutable state, so sharing one instance across workers is safe.
    static const std::regex kComputeLocalSizePattern(R"(local_size_([xyz])\s*=\s*([0-9]+))");

    static ComputeLocalSize ParseComputeLocalSize(const MobileGL::String& source) {
        ComputeLocalSize localSize;
        const MobileGL::String uncommentedSource = StripGlslComments(source);

        for (std::sregex_iterator it(uncommentedSource.begin(), uncommentedSource.end(), kComputeLocalSizePattern),
             end;
             it != end; ++it) {
            const char axis = (*it)[1].str()[0];
            // The [0-9]+ capture is unbounded, so `local_size_x = 99999999999999999999999`
            // is a legal match. std::stoull would throw std::out_of_range on it and let the
            // exception escape glCompileShader; std::from_chars reports the overflow instead.
            // An overflowing literal saturates to UINT_MAX, which the device-limit check
            // below rejects anyway - the same verdict a non-overflowing huge value gets.
            const MobileGL::String digits = (*it)[2].str();
            unsigned long long value = 0;
            const std::from_chars_result parsed =
                std::from_chars(digits.data(), digits.data() + digits.size(), value);
            const MobileGL::Uint clampedValue = (parsed.ec != std::errc() || value > UINT_MAX)
                                                    ? UINT_MAX
                                                    : static_cast<MobileGL::Uint>(value);

            // TODO: Replace this literal layout scanner with parser/AST-backed validation so expressions and
            // specialization-id layouts are handled consistently with glslang.
            localSize.declared = true;
            if (axis == 'x') {
                localSize.x = clampedValue;
            } else if (axis == 'y') {
                localSize.y = clampedValue;
            } else {
                localSize.z = clampedValue;
            }
        }

        return localSize;
    }

    // The device limits come from the CompileEnv snapshot, never from a live driver query.
    // GL_MAX_COMPUTE_WORK_GROUP_SIZE is a real GLES call on the DirectGLES backend: issued
    // off the context thread it would silently no-op and turn a legal local_size_z into
    // COMPILE_STATUS=FALSE. CaptureCompileEnv() issues it once, on the GL thread.
    static std::optional<MobileGL::String> ValidateComputeLocalSizeLimits(
        const MobileGL::String& source, const MobileGL::MG_Util::ShaderTranspiler::CompileEnv& env) {
        const ComputeLocalSize localSize = ParseComputeLocalSize(source);
        if (!localSize.declared) return std::nullopt;

        if (localSize.x > env.maxComputeWorkGroupSize[0] || localSize.y > env.maxComputeWorkGroupSize[1] ||
            localSize.z > env.maxComputeWorkGroupSize[2]) {
            return "Compute shader local_size exceeds GL_MAX_COMPUTE_WORK_GROUP_SIZE.";
        }

        const unsigned long long invocations = static_cast<unsigned long long>(localSize.x) * localSize.y * localSize.z;
        if (invocations > env.maxComputeWorkGroupInvocations) {
            return "Compute shader local_size product exceeds GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS.";
        }

        return std::nullopt;
    }

    // What glGetIntegerv(GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS) answers, recomputed rather than
    // queried: the compile runs on a worker with no context, and the pname is not a plain backend
    // parameter - the getter caps the backend's count by the state layer's fixed binding-point
    // array (GL_Getter's GetIndexedBufferQueryPointCount). A shader must be judged against the
    // number the application was told, not against either half of it.
    static MobileGL::Int MaxShaderStorageBufferBindings(
        const MobileGL::MG_Util::ShaderTranspiler::CompileEnv& env) {
        const MobileGL::Int frontendPoints =
            static_cast<MobileGL::Int>(MobileGL::MG_State::GLState::BufferBindingPointCount);
        if (!env.HasBackend()) return frontendPoints;
        return std::min<MobileGL::Int>(frontendPoints, std::max<MobileGL::Int>(env.params.MaxShaderStorageBufferBindings, 0));
    }

    // The half of a compile that depends on nothing but the source text, the stage and the
    // environment snapshot: preprocessing and the three lexical rejections. Split out so P0b
    // layer 2 can memoize exactly this and nothing else - the glslang parse stays per-object
    // because its TShader is consume-once. Deliberately free of any per-object state so the
    // memo is sound.
    //
    // The side-channel EXTRACTIONS that used to live here are gone: what the relaxed parse
    // destroys is now recovered from glslang itself, at the two points where it is destroyed
    // (see ShaderCompileArtifacts::explicitUniformLocations and
    // TMglGlslIoResolver::reserverResourceSlot). They could not stay here anyway - none of
    // them is a function of the unexpanded source text, which is all this half can see.
    //
    // The compute local-size verdict reads `env` rather than the live backend, and
    // env.fingerprint is part of the P0b cache key, so a memo can never be returned against
    // limits other than the ones it was computed against.
    static MobileGL::MG_State::GLState::ShaderPreprocessResult RunSourceOnlyPipeline(
        const MobileGL::ShaderStage stage, const MobileGL::String& source,
        const MobileGL::MG_Util::ShaderTranspiler::CompileEnv& env) {
        using namespace MobileGL;
        using namespace MobileGL::MG_Util::ShaderTranspiler;
        using MobileGL::MG_State::GLState::ShaderPreprocessOutcome;

        MobileGL::MG_State::GLState::ShaderPreprocessResult result;
        result.preprocessedSource = source;
        PreprocessShaderSource(stage, result.preprocessedSource, env);

        if (stage == ShaderStage::Compute) {
            if (const std::optional<String> localSizeError =
                    ValidateComputeLocalSizeLimits(result.preprocessedSource, env)) {
                result.outcome = ShaderPreprocessOutcome::ComputeLocalSizeRejected;
                result.infoLog = *localSizeError;
                return result;
            }
        }

        if (const std::optional<String> reservedError = FindReservedIdentifierViolation(result.preprocessedSource)) {
            result.outcome = ShaderPreprocessOutcome::ReservedIdentifierRejected;
            result.infoLog = *reservedError;
            return result;
        }

        if (const std::optional<String> bindingError = FindShaderStorageBindingViolation(
                result.preprocessedSource, MaxShaderStorageBufferBindings(env))) {
            result.outcome = ShaderPreprocessOutcome::ResourceBindingRejected;
            result.infoLog = *bindingError;
            return result;
        }

        // NO ATOMIC-COUNTER OFFSET SCAN HERE ANY MORE: glslang raises both rules itself now, at
        // the site where its relaxed remap folds the counter into a synthesized block
        // (ParseHelper.cpp atomicCounterOffsetCheck, called from vkRelaxedRemapUniformVariable).
        // A violation is an ordinary parse failure, so it reaches GL through the same path every
        // other compile error does - and, unlike a scan of unexpanded text, it sees an offset
        // spelled as a macro or a const expression.

        // The parse this feeds runs in the link-compatible configuration (Vulkan-client
        // env with relaxed rules): the TShader it produces is what glLinkProgram links and
        // what the backends' SPIR-V is generated from - there is no second, GL-client
        // parse. The GL frontend semantics the relaxed parse cannot provide are restored
        // on top, all of them out of glslang: explicit default-block uniform locations from
        // the snapshot the parse takes, opaque bindings and unqualified storage blocks from
        // the IO mapper's collect callback, dead-uniform/global-UBO filtering in
        // ProgramObject::DoReflection.
        result.outcome = ShaderPreprocessOutcome::Preprocessed;
        return result;
    }

} // namespace

namespace MobileGL::MG_State::GLState {
    // glslang has no "detach this thread" API in the vendored revision (there is no
    // InitThread/DetachThread pair any more; thread attachment is implicit through
    // thread_local state, and glslang::InitializeProcess() is process-wide, refcounted and
    // mutex-guarded, so it needs no per-worker counterpart). The pool allocator is the part
    // that needs undoing; see the declaration in ShaderCompileTask.h.
    GlslangThreadAllocatorGuard::~GlslangThreadAllocatorGuard() { glslang::SetThreadPoolAllocator(nullptr); }

    // Pure CPU work only. Everything this reads is either an input the node owns or a
    // process-wide constant; everything it writes is `artifacts`. Do not add a GL/EGL call,
    // a pActiveBackendObject read, or a pGLContext->RecordError() here - the first two are
    // what CompileEnv exists to replace, and the third is why the design's section 6
    // deferral mechanism (and JobNode's debug assert on it) exists.
    void ShaderCompileTask::RunBody() {
        // Own the failure rather than letting JobNode's backstop settle the node as
        // Cancelled: an abandoned node publishes nothing, so the shader would report
        // COMPILE_STATUS false with an EMPTY info log. GL models a failed compile as
        // status + log, so turn a throw into exactly that - a completed job whose result
        // is "this shader did not compile", with a log the application can read.
        // (JobNode still catches: it is the last resort for anything below.)
        try {
            RunCompilePipeline();
        } catch (const std::exception& e) {
            artifacts = {};
            artifacts.env = env;
            artifacts.compileStatus = false;
            artifacts.infoLog = std::format("Error: shader compilation failed: {}", e.what());
        } catch (...) {
            artifacts = {};
            artifacts.env = env;
            artifacts.compileStatus = false;
            artifacts.infoLog = "Error: shader compilation failed: unknown exception";
        }
    }

    void ShaderCompileTask::RunCompilePipeline() {
        using namespace MG_Util::ShaderTranspiler;
        const GlslangThreadAllocatorGuard glslangGuard;

        const CompileEnv& compileEnv = *env;
        artifacts.env = env;

        // P0b layer 2: another shader object in this context may already have run the
        // source-only half over byte-identical text under the same environment.
        ShaderPreprocessResultPtr cached =
            cache ? cache->Find(stage, sourceHash, *source, compileEnv.fingerprint) : nullptr;
        SharedPtr<ShaderPreprocessResult> fresh;
        if (!cached) fresh = MakeShared<ShaderPreprocessResult>(RunSourceOnlyPipeline(stage, *source, compileEnv));
        const ShaderPreprocessResult& shared = cached ? *cached : *fresh;
        const Bool shouldPopulateCache = !cached && cache != nullptr;

        if (!shared.Preprocessed()) {
            // Rejected lexically, or a glslang failure this context has already seen for
            // this exact source (ParseFailed) - either way the parse can be skipped.
            artifacts.infoLog = shared.infoLog;
            if (shouldPopulateCache) {
                cache->Insert(stage, sourceHash, *source, compileEnv.fingerprint, Move(fresh));
            }
            return;
        }

        const GLenum glShaderType = MG_Util::ConvertShaderStageToGLEnum(stage);
        // Always 0 on both production parse paths; see the key inventory on
        // ShaderParseVerdictKeyInputs for why it is in the key regardless.
        constexpr Uint32 kShaderCompileFlags = 0;

        // ---- L1c of the shader translation memo: the PARSE VERDICT ----------------------
        // Everything below this probe - the glslang parse itself - is what a hit skips. What
        // a hit does NOT produce is a TShader, and that is deliberate rather than a
        // limitation: the TShader is consume-once, so it could never have been shared, and
        // nothing on the COMPILE side of GL reads it. GL_COMPILE_STATUS, the info log,
        // GL_SHADER_SOURCE, attach/detach and reuse across programs are all answered from
        // what the verdict and the source-only half already carry.
        //
        // The parse is not skipped, it is DEFERRED: ClaimParsedShader re-parses on demand
        // when a link finds no stored parse. A link that hits L1 never asks, so the parse
        // never happens at all; a link that misses pays exactly one parse, where the CAS
        // loser has always paid it. See TranslationCache.h's L1c section.
        const TranslationCacheKey parseKey =
            ShaderTranslationCacheEnabled()
                ? BuildShaderParseVerdictKey(ShaderParseVerdictKeyInputs{
                      .frontendFingerprint = compileEnv.frontendFingerprint,
                      .shaderType = glShaderType,
                      .preprocessedSource = StringView(shared.preprocessedSource),
                      .shaderCompileFlags = kShaderCompileFlags})
                : TranslationCacheKey{};
        const ShaderParseVerdictPtr verdict =
            parseKey.Valid() ? GetShaderParseVerdictCache().Find(parseKey) : nullptr;

        // The two branches produce exactly one thing between them - a verdict, plus a TShader
        // only when this task actually parsed - and converge on one publish below. Keeping the
        // publish common is what stops a hit and a miss from ever drifting on WHAT a compile
        // makes observable.
        Bool parsedOk = false;
        String parseLog;
        SharedPtr<glslang::TShader> parsedShader;
        UnorderedMap<String, Int> explicitUniformLocations;

        if (verdict) {
            parsedOk = verdict->parsed;
            parseLog = verdict->infoLog;
            // From the verdict, not from a parse - see ShaderParseVerdict for why they had to
            // move into the payload when their origin moved into glslang.
            explicitUniformLocations = verdict->explicitUniformLocations;
            MGLOG_D("ShaderCompileTask: shader %u (stage %d) L1c hit - the glslang parse was skipped; "
                    "compileStatus = %d",
                    externalIndex, static_cast<Int>(stage), static_cast<Int>(parsedOk));
        } else {
            const ShaderAttrib attrib{.shaderType = glShaderType,
                                      .sourceStr = shared.preprocessedSource,
                                      .flags = kShaderCompileFlags,
                                      .env = &compileEnv};
            auto result = ShaderCompiler::CompileShader(attrib);
            parsedOk = result.has_value();
            if (parsedOk) {
                parsedShader = result.value();
                explicitUniformLocations = CollectExplicitUniformLocations(*parsedShader);
            } else {
                parseLog = result.error().log;
            }
            if (parseKey.Valid()) {
                auto freshVerdict = MakeShared<ShaderParseVerdict>();
                freshVerdict->parsed = parsedOk;
                // Empty on success by construction, matching what the publish below does with
                // the artifacts' own log; the diagnostic the application reads on failure.
                freshVerdict->infoLog = parseLog;
                freshVerdict->explicitUniformLocations = explicitUniformLocations;
                const SizeT verdictBytes = ShaderParseVerdictBytes(*freshVerdict);
                GetShaderParseVerdictCache().Insert(parseKey, ShaderParseVerdictPtr(Move(freshVerdict)),
                                                    verdictBytes);
            }
        }

        if (parsedOk) {
            artifacts.compileStatus = true;
            // NULL ON AN L1c HIT, and that is a supported state rather than an oversight: see
            // ShaderCompileArtifacts::shader and ClaimParsedShader.
            artifacts.shader = Move(parsedShader);
            // Copy, not move: `shared` may alias a cache entry that has to outlive us, and
            // `fresh` is about to be handed to the cache. Populated on the hit path too - it
            // is what ClaimParsedShader's deferred parse consumes.
            artifacts.preprocessedSource = shared.preprocessedSource;
            artifacts.explicitUniformLocations = Move(explicitUniformLocations);
            artifacts.infoLog.clear();
            if (shouldPopulateCache) {
                cache->Insert(stage, sourceHash, *source, compileEnv.fingerprint, Move(fresh));
            }
        } else {
            artifacts.infoLog = Move(parseLog);
            // Deferred, not logged here, for two reasons. MGLOG from a pool thread interleaves
            // mid-line with the GL thread's own output and lands out of order relative to the
            // glCompileShader that caused it; diagnostics.logLines is replayed by the join, on
            // the GL thread, exactly where a serial implementation would have printed it.
            // And a one-line summary rather than the old full source dump: a shaderpack stage
            // is ~100KB, so the dump was the single largest thing this driver ever wrote to
            // the log, for every failing shader. The info log is what names the offending
            // line; the source is recoverable from the application.
            const SizeT firstLineEnd = artifacts.infoLog.find('\n');
            diagnostics.logLines.push_back(std::format(
                "ShaderCompileTask: shader {} (stage {}) failed to compile; compileStatus = false. "
                "Preprocessed source: {} bytes. First log line: {}",
                externalIndex, static_cast<Int>(stage), shared.preprocessedSource.length(),
                artifacts.infoLog.substr(0, firstLineEnd == String::npos ? artifacts.infoLog.length()
                                                                        : firstLineEnd)));
            if (shouldPopulateCache) {
                fresh->outcome = ShaderPreprocessOutcome::ParseFailed;
                fresh->infoLog = artifacts.infoLog;
                cache->Insert(stage, sourceHash, *source, compileEnv.fingerprint, Move(fresh));
            }
        }
    }

    SharedPtr<glslang::TShader> ShaderCompileTask::ClaimParsedShader(String& outReparseLog) const {
        MOBILEGL_ASSERT(IsComplete(),
                        "ShaderCompileTask::ClaimParsedShader() on a job that has not completed; its artifacts "
                        "are still being written");

        if (artifacts.shader) {
            // The whole race, in one instruction. Acquire-release because the winner is about
            // to hand the TShader to glslang's linker on a possibly different thread from the
            // one that parsed it - the node's terminal transition already published the
            // parse, and this orders the two claimants against each other.
            Bool expected = false;
            if (m_parseClaimed.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                       std::memory_order_acquire)) {
                return artifacts.shader;
            }
        }

        // Three ways to be here: another link already consumed the stored parse (and mapIO
        // mutated its intermediate); the compile hit L1c and never parsed at all; or there
        // simply never was one. All three want the same thing - parse the preprocessed source
        // through the identical configuration. That costs one glslang parse, which is what
        // GenerateBinary used to spend here on EVERY link rather than only when needed.
        //
        // The guard is not optional on this path: from stage 4 this runs on a pool worker,
        // and TShader::parse would leave that worker's TLS allocator pointing at a pool the
        // GL thread is about to free. (ProgramLinkTask::RunBody holds one too; they nest
        // harmlessly - both just reset the thread to its own default.)
        const GlslangThreadAllocatorGuard glslangGuard;
        using namespace MG_Util::ShaderTranspiler;
        ShaderAttrib attrib{.shaderType = MG_Util::ConvertShaderStageToGLEnum(stage),
                            .sourceStr = artifacts.preprocessedSource,
                            .flags = 0,
                            // Re-parse against the SAME environment the original parse used,
                            // not against whatever the backend reports now.
                            .env = artifacts.env.get()};
        auto result = ShaderCompiler::CompileShader(attrib);
        if (!result) {
            // Should be unreachable. This exact (stage, preprocessed source, front-end env)
            // parsed successfully once - either at this node's own Compile(), or at the
            // Compile() whose verdict L1c handed this node - and every input the parse reads
            // is covered by that tuple. ConsumeShaders turns a null into a failed link with a
            // named internal error rather than a crash, which is the right shape for a
            // "cannot happen" that would otherwise be a silent miscompile.
            outReparseLog = result.error().log;
            return nullptr;
        }
        return result.value();
    }
} // namespace MobileGL::MG_State::GLState
