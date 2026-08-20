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
// Four pairs of cases, each Off/On:
//
//   ProgramLink      - the whole glCompileShader + glLinkProgram path for one program, with
//                      FRESH SHADER OBJECTS every iteration. This is the CTS shape exactly,
//                      and it is the headline case now. It used to be the PESSIMISTIC one:
//                      a hit still paid for both glslang parses, because the parse happens
//                      at glCompileShader - a different entry point from the one L1
//                      memoizes - and fresh shader objects meant ShaderCompileAdoptionMap
//                      could not hand the earlier parse over either. L1c is what closed
//                      that: the compile half of the memo recognises each stage's source
//                      and publishes its verdict without parsing, so on a hit this case now
//                      constructs no glslang object at all.
//
//   SharedShaderLink - the same program population with the shader objects KEPT ALIVE, so
//                      the parses happen once outside the measured loop whatever the cache
//                      does. That makes it the CONTROL for L1c rather than a target: its
//                      numbers should not move, and if they do, L1c has added cost to a
//                      path it was supposed to leave alone.
//
//   DeferredParseLink - the shape where L1c could LOSE: a constant vertex source (which
//                      hits L1c and therefore skips its parse) against a fresh fragment
//                      source every iteration (which makes the PROGRAM key miss, so the
//                      skipped parse has to happen inside the link after all). Same parse
//                      count either way, so the pair should land within noise; see its own
//                      header below.
//
//   EsslTranspile    - the DirectGLES backend segment: the SPIR-V pass chain plus
//                      SPIRV-Cross. Runs the driver-INDEPENDENT half of the real chain (the
//                      passes SyncToBackend runs unconditionally, plus the two stage-gated
//                      ones a fragment module reaches) so the miss path costs what
//                      production costs; the capability-gated passes need a live ES driver
//                      and are not reachable from a benchmark process.
//
// Every On case runs with a warm cache: the first iteration misses and every one after it
// hits, which is exactly the steady state of a 2592-program smoke case.

#include <benchmark/benchmark.h>

#include <string>

#include "Config.h"
#include "Includes.h"
#include "Init.h"
#include "MG_Impl/GLImpl/Program/GL_Program.h"
#include "MG_State/GLState/Core.h"
#include "MG_State/GLState/ProgramState/ProgramTranslationCache.h"
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
    const TranslationCacheStats before = MG_State::GLState::GetProgramTranslationCache().Stats();
    const TranslationCacheStats parseBefore = GetShaderParseVerdictCache().Stats();
    for (auto _ : state) {
        LinkOneProgram(vs, fs);
    }
    const TranslationCacheStats stats = MG_State::GLState::GetProgramTranslationCache().Stats();
    const TranslationCacheStats parseStats = GetShaderParseVerdictCache().Stats();
    state.counters["L1_hits"] = static_cast<double>(stats.hits - before.hits);
    state.counters["L1_misses"] = static_cast<double>(stats.misses - before.misses);
    // Two stages per iteration, so a clean run shows L1c_hits == 2 * iterations and zero
    // misses: every glCompileShader in the loop skipped its parse.
    state.counters["L1c_hits"] = static_cast<double>(parseStats.hits - parseBefore.hits);
    state.counters["L1c_misses"] = static_cast<double>(parseStats.misses - parseBefore.misses);
}
BENCHMARK(BM_ProgramLink_CacheOn)->Arg(0)->Arg(120)->Unit(benchmark::kMicrosecond);

// ---------------------------------------------------------------------------------------
// L1, the shape the memo actually exists for: MANY PROGRAMS OUT OF THE SAME SHADERS.
//
// The pair above deletes its shader objects every iteration, which forces a fresh glslang
// parse per iteration no matter what the link does - glCompileShader parses, and that is a
// DIFFERENT entry point from the one L1 memoizes. It is a real workload (what an application
// that never reuses a shader object pays) but it is the pessimistic one, and the residual it
// leaves is the parse, not the link.
//
// This pair keeps the shader objects alive, so the parses happen once before the measured
// loop and the L1 hit then skips the link, mapIO, the SPIR-V, the reflection and the routing
// outright.
//
// SINCE L1c THIS IS THE CONTROL, NOT THE TARGET. Nothing inside the measured loop calls
// glCompileShader, so L1c cannot fire here at all - which is exactly what makes the pair
// useful: it is the shape that says whether the compile-side memo has slowed the LINK path
// down. Its numbers should be indistinguishable from the pre-L1c ones.
// ---------------------------------------------------------------------------------------
namespace {
    struct SharedShaders {
        GLuint vs = 0;
        GLuint fs = 0;
    };

    SharedShaders MakeSharedShaders(const String& vertexSource, const String& fragmentSource) {
        using namespace MG_Impl::GLImpl;
        SharedShaders shaders;
        shaders.vs = CreateShader(GL_VERTEX_SHADER);
        const char* vsText = vertexSource.c_str();
        ShaderSource(shaders.vs, 1, &vsText, nullptr);
        CompileShader(shaders.vs);
        shaders.fs = CreateShader(GL_FRAGMENT_SHADER);
        const char* fsText = fragmentSource.c_str();
        ShaderSource(shaders.fs, 1, &fsText, nullptr);
        CompileShader(shaders.fs);
        return shaders;
    }

    void LinkFromSharedShaders(const SharedShaders& shaders) {
        using namespace MG_Impl::GLImpl;
        const GLuint program = CreateProgram();
        AttachShader(program, shaders.vs);
        AttachShader(program, shaders.fs);
        LinkProgram(program);
        benchmark::DoNotOptimize(program);
        DeleteProgram(program);
    }
} // namespace

static void BM_SharedShaderLink_CacheOff(benchmark::State& state) {
    MobileGL::Initialize();
    const SyncCompileScope sync;
    const CacheModeScope cache(false);
    const SharedShaders shaders =
        MakeSharedShaders(kVertexSource, SwizzleLikeFragment("", static_cast<int>(state.range(0))));
    for (auto _ : state) {
        LinkFromSharedShaders(shaders);
    }
    state.SetLabel("MOBILEGL_SHADER_CACHE=0");
}
BENCHMARK(BM_SharedShaderLink_CacheOff)->Arg(0)->Arg(120)->Unit(benchmark::kMicrosecond);

static void BM_SharedShaderLink_CacheOn(benchmark::State& state) {
    MobileGL::Initialize();
    const SyncCompileScope sync;
    const CacheModeScope cache(true);
    const SharedShaders shaders =
        MakeSharedShaders(kVertexSource, SwizzleLikeFragment("", static_cast<int>(state.range(0))));
    LinkFromSharedShaders(shaders); // prime, so the measured loop is the steady state
    const TranslationCacheStats before = MG_State::GLState::GetProgramTranslationCache().Stats();
    for (auto _ : state) {
        LinkFromSharedShaders(shaders);
    }
    const TranslationCacheStats stats = MG_State::GLState::GetProgramTranslationCache().Stats();
    state.counters["L1_hits"] = static_cast<double>(stats.hits - before.hits);
    state.counters["L1_misses"] = static_cast<double>(stats.misses - before.misses);
}
BENCHMARK(BM_SharedShaderLink_CacheOn)->Arg(0)->Arg(120)->Unit(benchmark::kMicrosecond);

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

// ---------------------------------------------------------------------------------------
// L1c, the shape where it could LOSE rather than win: the DEFERRED PARSE.
// ---------------------------------------------------------------------------------------
// A stage whose compile hits L1c holds no AST, so if the program-level key then MISSES, the
// parse it skipped has to happen anyway - inside the link, via ClaimParsedShader. The parse
// is moved, not removed, and this pair is what says whether moving it costs anything.
//
// The shape forces exactly that, every iteration: one CONSTANT vertex source (hits L1c after
// the first iteration) linked against a FRESH fragment source each time (misses L1c, and
// makes the program key miss too). So:
//
//   cache off - two parses at glCompileShader, then the link.
//   cache on  - one parse at glCompileShader (the fragment), one deferred parse inside the
//               link (the vertex), then the link.
//
// The parse count is identical, so these two should land within noise of each other. If the
// On arm is materially SLOWER, L1c is charging for something - the per-compile key build and
// hash over the full preprocessed source, or the loss of the claim-CAS reuse - and that cost
// shows up here and nowhere else.
//
// The distinct fragment sources also churn both front-end levels through their FIFO caps,
// which is the eviction behaviour a real shaderpack load produces; over a long run the
// constant vertex entry is occasionally evicted by that churn and re-inserted, so the L1c
// hit rate reported below is high but not exactly 1.0 per iteration.
namespace {
    String UniqueFragmentSource(const Uint64 serial, const int padLines) {
        return SwizzleLikeFragment("", padLines) +
               "\n// unique-" + std::to_string(serial) + "\n";
    }
} // namespace

static void BM_DeferredParseLink_CacheOff(benchmark::State& state) {
    MobileGL::Initialize();
    const SyncCompileScope sync;
    const CacheModeScope cache(false);
    const String vs = kVertexSource;
    Uint64 serial = 0;
    for (auto _ : state) {
        LinkOneProgram(vs, UniqueFragmentSource(serial++, static_cast<int>(state.range(0))));
    }
    state.SetLabel("MOBILEGL_SHADER_CACHE=0");
}
BENCHMARK(BM_DeferredParseLink_CacheOff)->Arg(0)->Arg(120)->Unit(benchmark::kMicrosecond);

static void BM_DeferredParseLink_CacheOn(benchmark::State& state) {
    MobileGL::Initialize();
    const SyncCompileScope sync;
    const CacheModeScope cache(true);
    const String vs = kVertexSource;
    Uint64 serial = 0;
    LinkOneProgram(vs, UniqueFragmentSource(~0ull, static_cast<int>(state.range(0)))); // prime the vertex entry
    const TranslationCacheStats before = MG_State::GLState::GetProgramTranslationCache().Stats();
    const TranslationCacheStats parseBefore = GetShaderParseVerdictCache().Stats();
    for (auto _ : state) {
        LinkOneProgram(vs, UniqueFragmentSource(serial++, static_cast<int>(state.range(0))));
    }
    const TranslationCacheStats stats = MG_State::GLState::GetProgramTranslationCache().Stats();
    const TranslationCacheStats parseStats = GetShaderParseVerdictCache().Stats();
    // Expected shape: L1 all misses (every program is new), L1c one hit (vertex) and one miss
    // (fragment) per iteration.
    state.counters["L1_hits"] = static_cast<double>(stats.hits - before.hits);
    state.counters["L1_misses"] = static_cast<double>(stats.misses - before.misses);
    state.counters["L1c_hits"] = static_cast<double>(parseStats.hits - parseBefore.hits);
    state.counters["L1c_misses"] = static_cast<double>(parseStats.misses - parseBefore.misses);
}
BENCHMARK(BM_DeferredParseLink_CacheOn)->Arg(0)->Arg(120)->Unit(benchmark::kMicrosecond);

BENCHMARK_MAIN();
