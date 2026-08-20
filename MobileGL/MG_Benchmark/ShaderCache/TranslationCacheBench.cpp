// MobileGL - MobileGL/MG_Benchmark/ShaderCache/TranslationCacheBench.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// What the two-level shader translation memo is worth, measured on the workload that
// motivated it: the KHR-GL33.texture_swizzle.smoke_* shape, where one case builds 2592
// programs out of a handful of distinct sources.
//
// Two pairs of cases, each Off/On:
//
//   ProgramLink   - the whole glCompileShader + glLinkProgram path for one program, in
//                   situ. This is what L1 moves, and it is deliberately the PESSIMISTIC
//                   number: a hit still pays for both glslang parses and the link, because
//                   the frontend's GL query surface is built out of the TProgram they
//                   produce. Only GlslangToSpv and the 11-pass sanitize chain are skipped.
//
//   EsslTranspile - the DirectGLES backend segment: the SPIR-V pass chain plus SPIRV-Cross.
//                   Runs the driver-INDEPENDENT half of the real chain (the passes
//                   SyncToBackend runs unconditionally, plus the two stage-gated ones a
//                   fragment module reaches) so the miss path costs what production costs;
//                   the capability-gated passes need a live ES driver and are not
//                   reachable from a benchmark process.
//
// Both On cases run with a warm cache: the first iteration misses and every one after it
// hits, which is exactly the steady state of a 2592-program smoke case.

#include <benchmark/benchmark.h>

#include <string>

#include "Config.h"
#include "Includes.h"
#include "Init.h"
#include "MG_Impl/GLImpl/Program/GL_Program.h"
#include "MG_State/GLState/Core.h"
#include "MG_Util/ShaderTranspiler/ShaderCompiler.h"
#include "MG_Util/ShaderTranspiler/SpvcSession.h"
#include "MG_Util/ShaderTranspiler/TranslationCache.h"
#include "MG_Util/ShaderTranspiler/Types.h"

using namespace MobileGL;
using namespace MobileGL::MG_Util::ShaderTranspiler;

namespace {
    const char* kVertexSource = R"(#version 460
layout(location = 0) in vec3 aPos;
out vec3 vPos;
out vec2 vUv;
void main() {
    vPos = aPos;
    vUv = aPos.xy * 0.5 + 0.5;
    gl_Position = vec4(aPos, 1.0);
}
)";

    // Shaped after gl3cTextureSwizzleTests.cpp's template: a sampler of one type, one
    // TEXTURE_ACCESS, one CHANNEL, and an output whose BASIC_TYPE is the only thing that
    // varies within a case. Padded with enough real arithmetic that the translation chain
    // is doing work rather than measuring fixed overheads.
    // `padLines` = 0 is the honest CTS size: gl3cTextureSwizzleTests' smoke template is a
    // handful of lines, and that is the workload the memo exists for. The padded variant is
    // kept alongside it because a shaderpack stage is orders of magnitude bigger, and the
    // two bracket the ratio the cache is worth in practice.
    String SwizzleLikeFragment(const String& prefix, const int padLines) {
        String source = "#version 460\n";
        source += "in vec3 vPos;\n";
        source += "in vec2 vUv;\n";
        source += "layout(location = 0) out " + prefix + "vec4 fragColor;\n";
        source += "uniform sampler2D uTex;\n";
        source += "uniform vec4 uTint;\n";
        source += "uniform mat4 uModel;\n";
        source += "uniform float uArr[8];\n";
        source += "void main() {\n";
        source += "    vec4 s = texture(uTex, vUv);\n";
        source += "    float acc = s.r;\n";
        for (int i = 0; i < padLines; ++i) {
            source += "    acc = acc * 1.0001 + sin(acc + " + std::to_string(i) + ".0) * cos(acc);\n";
        }
        source += "    for (int i = 0; i < 8; ++i) acc += uArr[i];\n";
        source += "    vec4 p = uModel * vec4(vPos, 1.0);\n";
        source += "    fragColor = " + prefix + "vec4((s + uTint) * acc + p);\n";
        source += "}\n";
        return source;
    }

    class CacheModeScope {
    public:
        explicit CacheModeScope(const Bool enabled)
            : m_saved(MG_Config::Features.ShaderTranslationCache) {
            MG_Config::Features.ShaderTranslationCache =
                enabled ? MG_Config::QuirkOverride::ForceOn : MG_Config::QuirkOverride::ForceOff;
        }
        ~CacheModeScope() { MG_Config::Features.ShaderTranslationCache = m_saved; }

    private:
        const MG_Config::QuirkOverride m_saved;
    };

    class SyncCompileScope {
    public:
        SyncCompileScope() : m_saved(MG_Config::Features.AsyncShaderCompile) {
            MG_Config::Features.AsyncShaderCompile = MG_Config::QuirkOverride::ForceOff;
        }
        ~SyncCompileScope() { MG_Config::Features.AsyncShaderCompile = m_saved; }

    private:
        const MG_Config::QuirkOverride m_saved;
    };

    // One program, built the way the CTS builds one: fresh shader objects every time.
    void LinkOneProgram(const String& vertexSource, const String& fragmentSource) {
        using namespace MG_Impl::GLImpl;
        const GLuint vs = CreateShader(GL_VERTEX_SHADER);
        const char* vsText = vertexSource.c_str();
        ShaderSource(vs, 1, &vsText, nullptr);
        CompileShader(vs);

        const GLuint fs = CreateShader(GL_FRAGMENT_SHADER);
        const char* fsText = fragmentSource.c_str();
        ShaderSource(fs, 1, &fsText, nullptr);
        CompileShader(fs);

        const GLuint program = CreateProgram();
        AttachShader(program, vs);
        AttachShader(program, fs);
        LinkProgram(program);
        benchmark::DoNotOptimize(program);

        DeleteProgram(program);
        DeleteShader(vs);
        DeleteShader(fs);
    }

    Vector<Uint32> BuildSanitizedFragmentSpirv(const String& fragmentSource) {
        ShaderAttrib attrib{.shaderType = GL_FRAGMENT_SHADER, .sourceStr = fragmentSource};
        auto shader = ShaderCompiler::CompileShader(attrib);
        if (!shader) return {};
        ProgramAttrib programAttrib{.shaders = {shader.value()}};
        auto program = ShaderCompiler::LinkProgram(programAttrib);
        if (!program) return {};
        ProgramBinaryAttrib binaryAttrib{.shaderTypes = {GL_FRAGMENT_SHADER}, .program = *program.value()};
        auto binary = ShaderCompiler::GetSpirvBinaryFromProgram(binaryAttrib);
        if (!binary || binary->empty()) return {};
        Vector<Uint32> sanitized;
        if (!ShaderCompiler::SanitizeAndOptimizeBinary(binary->front(), sanitized)) return {};
        return sanitized;
    }

    // The driver-independent part of BackendProgramObjectImpl::TranspileSpirvToEssl, in the
    // same order. What is missing is only the capability-gated passes (viewport lowering,
    // multisample clamping, noperspective emulation, the image-format bake), which cannot
    // fire without a live ES driver to arm them.
    Bool TranspileLikeDirectGles(const Vector<Uint32>& spirv, const Uint esslVersion, String& outEssl) {
        Vector<Uint32> a;
        const Vector<Uint32>* effective = &spirv;
        if (ShaderCompiler::StripUboMemberRelaxedPrecisionForEssl(*effective, a, false) && !a.empty()) {
            effective = &a;
        }
        Vector<Uint32> b;
        if (ShaderCompiler::LowerRectImages(*effective, b, false) && !b.empty()) effective = &b;
        Vector<Uint32> c;
        if (ShaderCompiler::Lower1DArrayImagesForEssl(*effective, c, false) && !c.empty()) effective = &c;
        Vector<Uint32> d;
        if (ShaderCompiler::LegalizeFragmentOutputIndexingForEssl(*effective, d, false) && !d.empty()) {
            effective = &d;
        }

        SpvcSession session(*effective, SessionUsageBit::Transpile);
        spvc_compiler_options options;
        if (session.CreateOptions(&options) != SPVC_SUCCESS) return false;
        spvc_compiler_options_set_uint(options, SPVC_COMPILER_OPTION_GLSL_VERSION, esslVersion);
        spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_GLSL_ES, SPVC_TRUE);
        spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_GLSL_VULKAN_SEMANTICS, SPVC_FALSE);
        session.SetOptions(options);
        const char* result = nullptr;
        session.Compile(&result);
        if (!result) return false;
        outEssl = result;
        return true;
    }

    EsslTranslationKeyInputs EsslInputsFor(const Vector<Uint32>& spirv) {
        EsslTranslationKeyInputs inputs;
        inputs.spirv = &spirv;
        inputs.shaderType = GL_FRAGMENT_SHADER;
        inputs.maxColorTextureSamples = 4;
        inputs.maxIntegerSamples = 1;
        inputs.maxDepthTextureSamples = 4;
        inputs.advertisedMaxSamples = 4;
        inputs.esslVersion = 320;
        return inputs;
    }
} // namespace

// ---------------------------------------------------------------------------------------
// L1, in situ: the full glCompileShader + glLinkProgram path for a repeated program.
// ---------------------------------------------------------------------------------------
// Arg(0) = the CTS smoke size; Arg(120) = a heavy stage, bracketing the ratio.
static void BM_ProgramLink_CacheOff(benchmark::State& state) {
    MobileGL::Initialize();
    const SyncCompileScope sync;
    const CacheModeScope cache(false);
    const String vs = kVertexSource;
    const String fs = SwizzleLikeFragment("", static_cast<int>(state.range(0)));
    for (auto _ : state) {
        LinkOneProgram(vs, fs);
    }
    state.SetLabel("MOBILEGL_SHADER_CACHE=0");
}
BENCHMARK(BM_ProgramLink_CacheOff)->Arg(0)->Arg(120)->Unit(benchmark::kMicrosecond);

static void BM_ProgramLink_CacheOn(benchmark::State& state) {
    MobileGL::Initialize();
    const SyncCompileScope sync;
    const CacheModeScope cache(true);
    const String vs = kVertexSource;
    const String fs = SwizzleLikeFragment("", static_cast<int>(state.range(0)));
    LinkOneProgram(vs, fs); // prime, so the measured loop is the steady state
    const TranslationCacheStats before = GetSpirvTranslationCache().Stats();
    for (auto _ : state) {
        LinkOneProgram(vs, fs);
    }
    const TranslationCacheStats stats = GetSpirvTranslationCache().Stats();
    state.counters["L1_hits"] = static_cast<double>(stats.hits - before.hits);
    state.counters["L1_misses"] = static_cast<double>(stats.misses - before.misses);
}
BENCHMARK(BM_ProgramLink_CacheOn)->Arg(0)->Arg(120)->Unit(benchmark::kMicrosecond);

// ---------------------------------------------------------------------------------------
// L2, component: the DirectGLES SPIR-V pass chain plus SPIRV-Cross for one stage.
// ---------------------------------------------------------------------------------------
static void BM_EsslTranspile_CacheOff(benchmark::State& state) {
    MobileGL::Initialize();
    const Vector<Uint32> spirv =
        BuildSanitizedFragmentSpirv(SwizzleLikeFragment("", static_cast<int>(state.range(0))));
    if (spirv.empty()) {
        state.SkipWithError("could not build the fragment module");
        return;
    }
    String essl;
    for (auto _ : state) {
        if (!TranspileLikeDirectGles(spirv, 320, essl)) {
            state.SkipWithError("transpile failed");
            break;
        }
        benchmark::DoNotOptimize(essl.data());
    }
    state.SetLabel("MOBILEGL_SHADER_CACHE=0");
}
BENCHMARK(BM_EsslTranspile_CacheOff)->Arg(0)->Arg(120)->Unit(benchmark::kMicrosecond);

static void BM_EsslTranspile_CacheOn(benchmark::State& state) {
    MobileGL::Initialize();
    const Vector<Uint32> spirv =
        BuildSanitizedFragmentSpirv(SwizzleLikeFragment("", static_cast<int>(state.range(0))));
    if (spirv.empty()) {
        state.SkipWithError("could not build the fragment module");
        return;
    }
    BoundedTranslationCache<EsslTranslationResult> cache("bench L2", 64, 8u << 20);
    const EsslTranslationKeyInputs inputs = EsslInputsFor(spirv);
    for (auto _ : state) {
        const TranslationCacheKey key = BuildEsslTranslationKey(inputs);
        EsslTranslationResultPtr hit = cache.Find(key);
        if (!hit) {
            auto payload = MakeShared<EsslTranslationResult>();
            if (!TranspileLikeDirectGles(spirv, inputs.esslVersion, payload->essl)) {
                state.SkipWithError("transpile failed");
                break;
            }
            cache.Insert(key, EsslTranslationResultPtr(payload), EsslTranslationResultBytes(*payload));
            hit = payload;
        }
        benchmark::DoNotOptimize(hit->essl.data());
    }
    const TranslationCacheStats stats = cache.Stats();
    state.counters["L2_hits"] = static_cast<double>(stats.hits);
    state.counters["L2_misses"] = static_cast<double>(stats.misses);
}
BENCHMARK(BM_EsslTranspile_CacheOn)->Arg(0)->Arg(120)->Unit(benchmark::kMicrosecond);

BENCHMARK_MAIN();
