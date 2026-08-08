// MobileGL - MobileGL/MG_State/GLState/ProgramState/ShaderObject.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include "ShaderObject.h"
#include "ShaderPreprocessCache.h"
#include <MG_Util/ShaderTranspiler/Types.h>
#include <MG_Util/ShaderTranspiler/ShaderCompiler.h>
#include <MG_Util/Converters/MGToGL/ProgramEnumConverter.h>
#include <MG_Util/ShaderTranspiler/ShaderSourceProcessor.h>
#include <MG_Util/ShaderTranspiler/glslang/UniformTraverser.h>
#include <MG_Backend/BackendObjects.h>

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
    // const regex carries no mutable state, so sharing one instance is safe.
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

    static MobileGL::Uint GetComputeWorkGroupSizeLimit(MobileGL::Uint index) {
        constexpr MobileGL::Uint kFrontendMinComputeWorkGroupSizes[] = {1024, 1024, 64};
        MobileGL::Int backendValue = 0;
        if (MobileGL::MG_Backend::gBackendFunctionsTable.GL.GetIntegeri_v) {
            MobileGL::MG_Backend::gBackendFunctionsTable.GL.GetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_SIZE, index,
                                                                          &backendValue);
        }

        // TODO: Share these exposed compute limit helpers with GL_Getter.cpp instead of duplicating the frontend minima.
        return std::max(static_cast<MobileGL::Uint>(std::max(backendValue, 0)),
                        kFrontendMinComputeWorkGroupSizes[index]);
    }

    static unsigned long long GetComputeWorkGroupInvocationLimit() {
        constexpr unsigned long long kFrontendMaxComputeWorkGroupInvocations = 1024;
        if (!MobileGL::MG_Backend::pActiveBackendObject) return kFrontendMaxComputeWorkGroupInvocations;

        return std::max(static_cast<unsigned long long>(std::max(
                            MobileGL::MG_Backend::pActiveBackendObject->GetDynamicParameters()
                                .MaxComputeWorkGroupInvocations,
                            0)),
                        kFrontendMaxComputeWorkGroupInvocations);
    }

    static std::optional<MobileGL::String> ValidateComputeLocalSizeLimits(const MobileGL::String& source) {
        const ComputeLocalSize localSize = ParseComputeLocalSize(source);
        if (!localSize.declared) return std::nullopt;

        if (localSize.x > GetComputeWorkGroupSizeLimit(0) || localSize.y > GetComputeWorkGroupSizeLimit(1) ||
            localSize.z > GetComputeWorkGroupSizeLimit(2)) {
            return "Compute shader local_size exceeds GL_MAX_COMPUTE_WORK_GROUP_SIZE.";
        }

        const unsigned long long invocations = static_cast<unsigned long long>(localSize.x) * localSize.y * localSize.z;
        if (invocations > GetComputeWorkGroupInvocationLimit()) {
            return "Compute shader local_size product exceeds GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS.";
        }

        return std::nullopt;
    }

    // The half of ShaderObject::Compile() that depends on nothing but the source text and
    // the stage: preprocessing, the two lexical rejections, and the two lexical
    // side-channel extractions. Split out so P0b layer 2 can memoize exactly this and
    // nothing else - the glslang parse stays per-object because its TShader is
    // consume-once. Deliberately free of any per-object state so the memo is sound.
    //
    // Caveat, documented rather than defended against: the compute local-size verdict also
    // reads the active backend's GL_MAX_COMPUTE_WORK_GROUP_* limits. Those are fixed for
    // the lifetime of a context, and the cache is per-context, so the memo cannot outlive
    // the limits it was computed against.
    static MobileGL::MG_State::GLState::ShaderPreprocessResult RunSourceOnlyPipeline(
        const MobileGL::ShaderStage stage, const MobileGL::String& source) {
        using namespace MobileGL;
        using namespace MobileGL::MG_Util::ShaderTranspiler;
        using MobileGL::MG_State::GLState::ShaderPreprocessOutcome;

        MobileGL::MG_State::GLState::ShaderPreprocessResult result;
        result.preprocessedSource = source;
        PreprocessShaderSource(stage, result.preprocessedSource);

        if (stage == ShaderStage::Compute) {
            if (const std::optional<String> localSizeError =
                    ValidateComputeLocalSizeLimits(result.preprocessedSource)) {
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

        // The parse this feeds runs in the link-compatible configuration (Vulkan-client
        // env with relaxed rules): the TShader it produces is what glLinkProgram links and
        // what the backends' SPIR-V is generated from - there is no second, GL-client
        // parse. The GL frontend semantics the relaxed parse cannot provide are restored
        // on top: explicit default-block uniform locations through the lexical
        // side-channels below, dead-uniform/global-UBO filtering in
        // ProgramObject::DoReflection.
        result.explicitUniformLocations = ExtractExplicitUniformLocations(result.preprocessedSource);
        result.explicitOpaqueBindings = ExtractExplicitOpaqueBindings(result.preprocessedSource);
        result.outcome = ShaderPreprocessOutcome::Preprocessed;
        return result;
    }
}

namespace MobileGL::MG_State::GLState {
    void ShaderObject::SetShaderSource(const String& source) {
        // P0b layer 1. glShaderSource always REPLACES the source, but replacing it with a
        // byte-identical one cannot change what a compile would produce: the whole
        // pipeline below (preprocess -> lexical checks -> glslang parse) is a pure
        // function of (stage, source) plus context-lifetime backend limits. So keeping the
        // compiled state is not an optimization that changes observable behaviour - the
        // COMPILE_STATUS, the info log and the reflection a caller can query are exactly
        // what a real recompile would have rebuilt, byte for byte.
        if (SourceMatchesCompiledState(source)) return;
        m_source = source;
        InvalidateCompiledState();
    }

    void ShaderObject::SetShaderSource(String&& source) {
        if (SourceMatchesCompiledState(source)) return;
        m_source = Move(source);
        InvalidateCompiledState();
    }

    Bool ShaderObject::SourceMatchesCompiledState(const String& candidate) const {
        if (!m_hasCompiledState) return false;
        if (candidate.length() != m_compiledSourceLength) return false;
        if (ShaderPreprocessCache::HashSource(candidate) != m_compiledSourceHash) return false;
        // The hash is a fast reject only; confirm against the actual stored text. While
        // m_hasCompiledState holds, m_source IS the source that produced the state.
        return candidate == m_source;
    }

    void ShaderObject::RememberCompiledSource(const Uint64 sourceHash) {
        m_hasCompiledState = true;
        m_compiledSourceHash = sourceHash;
        m_compiledSourceLength = m_source.length();
    }

    void ShaderObject::InvalidateCompiledState() {
        m_shader.reset();
        m_preprocessedSource.clear();
        m_explicitUniformLocations.clear();
        m_explicitOpaqueBindings.clear();
        m_shaderConsumedByLink = false;
        m_compileStatus = false;
        m_infoLog.clear();
        m_hasCompiledState = false;
        m_compiledSourceHash = 0;
        m_compiledSourceLength = 0;
    }

    void ShaderObject::Compile() {
        using namespace MG_Util::ShaderTranspiler;

        // P0b layer 1: the state this object holds was produced by a previous Compile() of
        // the exact source it still holds, so a recompile is a no-op. This covers the
        // failure case too - the info log stays queryable because nothing is cleared.
        //
        // m_shaderConsumedByLink interaction: if the stored TShader already fed a link,
        // the no-op leaves m_preprocessedSource and both side-channel maps intact, which
        // is precisely what TakeShaderForLink's on-demand re-parse needs. A real recompile
        // would have handed the next link a fresh parse; the no-op hands it a fresh
        // re-parse of the identical source instead. Same result, one parse either way.
        if (m_hasCompiledState) return;

        InvalidateCompiledState();

        const Uint64 sourceHash = ShaderPreprocessCache::HashSource(m_source);

        // P0b layer 2: another shader object in this context may already have run the
        // source-only half over byte-identical text.
        const ShaderPreprocessResult* cached =
            m_preprocessCache != nullptr ? m_preprocessCache->Find(m_stage, sourceHash, m_source) : nullptr;
        ShaderPreprocessResult fresh;
        if (cached == nullptr) fresh = RunSourceOnlyPipeline(m_stage, m_source);
        const ShaderPreprocessResult& shared = cached != nullptr ? *cached : fresh;
        const Bool shouldPopulateCache = cached == nullptr && m_preprocessCache != nullptr;

        if (!shared.Preprocessed()) {
            // Rejected lexically, or a glslang failure this context has already seen for
            // this exact source (ParseFailed) - either way the parse can be skipped.
            m_infoLog = shared.infoLog;
            if (shouldPopulateCache) m_preprocessCache->Insert(m_stage, sourceHash, m_source, Move(fresh));
            RememberCompiledSource(sourceHash);
            return;
        }

        ShaderAttrib attrib{.shaderType = MG_Util::ConvertShaderStageToGLEnum(m_stage),
                            .sourceStr = shared.preprocessedSource,
                            .flags = 0};

        auto result = ShaderCompiler::CompileShader(attrib);
        if (result) {
            m_compileStatus = true;
            m_shader = result.value();
            // Copy, not move: `shared` may alias a cache entry that has to outlive us, and
            // `fresh` is about to be handed to the cache.
            m_preprocessedSource = shared.preprocessedSource;
            m_explicitUniformLocations = shared.explicitUniformLocations;
            m_explicitOpaqueBindings = shared.explicitOpaqueBindings;
            m_infoLog.clear();
            if (shouldPopulateCache) m_preprocessCache->Insert(m_stage, sourceHash, m_source, Move(fresh));
        } else {
            m_infoLog = result.error().log;
            MGLOG_D("ShaderObject::Compile: Shader %d compilation failed.\nSource:\n%s\nInfoLog:\n%s\nSetting "
                    "m_compileStatus = false as a result.",
                    m_externalIndex, shared.preprocessedSource.c_str(), m_infoLog.c_str());
            if (shouldPopulateCache) {
                fresh.outcome = ShaderPreprocessOutcome::ParseFailed;
                fresh.infoLog = m_infoLog;
                fresh.explicitUniformLocations.clear();
                fresh.explicitOpaqueBindings.clear();
                m_preprocessCache->Insert(m_stage, sourceHash, m_source, Move(fresh));
            }
        }
        RememberCompiledSource(sourceHash);
    }

    SharedPtr<glslang::TShader> ShaderObject::TakeShaderForLink(String& outReparseLog) {
        if (m_shader && !m_shaderConsumedByLink) {
            m_shaderConsumedByLink = true;
            return m_shader;
        }

        // The stored parse already fed a link, whose mapIO mutated its intermediate.
        // Re-parse the preprocessed source through the identical configuration; this
        // costs one glslang parse, which is exactly what GenerateBinary used to spend
        // here on EVERY link rather than only on reuse.
        using namespace MG_Util::ShaderTranspiler;
        ShaderAttrib attrib{.shaderType = MG_Util::ConvertShaderStageToGLEnum(m_stage),
                            .sourceStr = m_preprocessedSource,
                            .flags = 0};
        auto result = ShaderCompiler::CompileShader(attrib);
        if (!result) {
            // Should be unreachable: the same source parsed successfully at Compile().
            outReparseLog = result.error().log;
            MGLOG_E("ShaderObject::TakeShaderForLink: re-parse of shader %d failed:\n%s", m_externalIndex,
                    outReparseLog.c_str());
            return nullptr;
        }
        return result.value();
    }

    void ShaderObject::MarkAsDeleted() {
        m_deleteStatus = true;
    }
} // namespace MobileGL::MG_State::GLState
