// MobileGL - MobileGL/MG_Test/ShaderTranspiler/TranslationCacheTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The two-level shader translation memo (MG_Util/ShaderTranspiler/TranslationCache.h).
//
// A wrong hit here is a silently miscompiled shader, so the cases below are weighted
// heavily towards the KEY rather than towards the plumbing: for each level there is one
// case per input that can change the output, asserting that moving that input alone moves
// the key. That is the test that catches an under-specified key, which is the only way
// this feature can produce a wrong answer.
//
// The rest covers the memo contract itself: FIFO eviction under both budgets, a hash
// collision degrading to a miss rather than to a wrong payload, the MOBILEGL_SHADER_CACHE
// escape hatch, and concurrent lookups/inserts over overlapping keys agreeing with the
// single-threaded answer.

#include <gtest/gtest.h>

#include <atomic>
#include <functional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "Config.h"
#include "Includes.h"
#include "Init.h"
#include "MG_Impl/GLImpl/Program/GL_Program.h"
#include "MG_State/GLState/Core.h"
#include "MG_State/GLState/ProgramState/ProgramTranslationCache.h"
#include "MG_Util/ShaderTranspiler/CompileEnv.h"
#include "MG_Util/ShaderTranspiler/ShaderCompiler.h"
#include "MG_Util/ShaderTranspiler/SpvcSession.h"
#include "MG_Util/ShaderTranspiler/TranslationCache.h"
#include "MG_Util/ShaderTranspiler/Types.h"

using namespace MobileGL;
using namespace MobileGL::MG_Util::ShaderTranspiler;

namespace {
    // Restores MOBILEGL_SHADER_CACHE's field on the way out, the same shape
    // AsyncSpirvPhaseTest's AsyncModeScope uses for its own toggle.
    class CacheModeScope {
    public:
        explicit CacheModeScope(const Bool enabled)
            : m_saved(MG_Config::Features.ShaderTranslationCache) {
            MG_Config::Features.ShaderTranslationCache =
                enabled ? MG_Config::QuirkOverride::ForceOn : MG_Config::QuirkOverride::ForceOff;
        }
        ~CacheModeScope() { MG_Config::Features.ShaderTranslationCache = m_saved; }
        CacheModeScope(const CacheModeScope&) = delete;
        CacheModeScope& operator=(const CacheModeScope&) = delete;

    private:
        const MG_Config::QuirkOverride m_saved;
    };

    // Synchronous links, so a case can read the L1 counters straight after LinkProgram
    // instead of having to join a phase-B job first.
    class SyncCompileScope {
    public:
        SyncCompileScope() : m_saved(MG_Config::Features.AsyncShaderCompile) {
            MG_Config::Features.AsyncShaderCompile = MG_Config::QuirkOverride::ForceOff;
        }
        ~SyncCompileScope() { MG_Config::Features.AsyncShaderCompile = m_saved; }
        SyncCompileScope(const SyncCompileScope&) = delete;
        SyncCompileScope& operator=(const SyncCompileScope&) = delete;

    private:
        const MG_Config::QuirkOverride m_saved;
    };

    struct TestPayload {
        String text;
    };

    TranslationCacheKey KeyFromText(const String& text) {
        TranslationKeyBuilder builder;
        builder.Text(text);
        return MakeTranslationCacheKey(builder);
    }

    SizeT PayloadBytes(const TestPayload& payload) { return payload.text.size(); }

    // ---- L1 fixtures ----
    const char* kVertexSource = R"(#version 460
layout(location = 0) in vec3 aPos;
out vec3 vPos;
void main() {
    vPos = aPos;
    gl_Position = vec4(aPos, 1.0);
}
)";

    const char* kFragmentSource = R"(#version 460
in vec3 vPos;
layout(location = 0) out vec4 fragColor;
uniform vec3 uTint;
void main() {
    fragColor = vec4(uTint * vPos, 1.0);
}
)";

    // Same shape as the KHR-GL33.texture_swizzle.smoke_* template: one substituted type, so
    // a case can build "the same shader again" and "a different shader" from one function.
    String SwizzleLikeFragment(const String& basicType) {
        return "#version 460\n"
               "in vec3 vPos;\n"
               "layout(location = 0) out " + basicType + "vec4 fragColor;\n"
               "uniform sampler2D uTex;\n"
               "void main() {\n"
               "    vec4 s = texture(uTex, vPos.xy);\n"
               "    fragColor = " + basicType + "vec4(s);\n"
               "}\n";
    }

    SpirvTranslationKeyInputs BaselineSpirvInputs(const Vector<SpirvTranslationKeyInputs::Stage>& stages) {
        SpirvTranslationKeyInputs inputs;
        inputs.frontendFingerprint = 0x1234'5678'9abc'def0ull;
        inputs.stages = stages;
        inputs.shaderCompileFlags = 0;
        inputs.enableSpirvValidation = false;
        inputs.xfbBufferMode = GL_INTERLEAVED_ATTRIBS;
        inputs.maxFragmentOutputColorNumber = 8;
        return inputs;
    }

    EsslTranslationKeyInputs BaselineEsslInputs(const Vector<Uint32>& spirv) {
        EsslTranslationKeyInputs inputs;
        inputs.spirv = &spirv;
        inputs.shaderType = GL_FRAGMENT_SHADER;
        inputs.supportsViewportArray = false;
        inputs.supportsNoperspectiveInterpolation = false;
        inputs.maxColorTextureSamples = 4;
        inputs.maxIntegerSamples = 1;
        inputs.maxDepthTextureSamples = 4;
        inputs.advertisedMaxSamples = 4;
        inputs.esslVersion = 320;
        inputs.enableSpirvValidation = false;
        return inputs;
    }

    Uint64 DigestOf(const Vector<Uint32>& words) {
        Uint64 hash = 1469598103934665603ull;
        for (const Uint32 word : words) hash = (hash ^ static_cast<Uint64>(word)) * 1099511628211ull;
        return hash;
    }

    Vector<Uint64> ProgramSpirvDigest(const GLuint program) {
        Vector<Uint64> digest;
        const auto& object = MG_State::pGLContext->GetProgramObject(program);
        if (!object) return digest;
        for (const auto& module : object->GetGeneratedSpirv()) digest.push_back(DigestOf(module));
        return digest;
    }

    // Everything the GL query surface says about a linked program, as one string. Used to
    // assert that a program served from the L1 memo - which never built a TProgram - answers
    // identically to the one that was parsed.
    String ReflectionFingerprint(const GLuint program) {
        const auto& object = MG_State::pGLContext->GetProgramObject(program);
        if (!object) return String();
        String out;
        const Uint uniformCount = object->GetUniformCount();
        for (Uint i = 0; i < uniformCount; ++i) {
            out += object->GetActiveUniformName(i);
            out += ':' + std::to_string(object->GetActiveUniformType(i));
            out += ':' + std::to_string(object->GetActiveUniformArraySize(i));
            out += ':' + std::to_string(object->GetActiveUniformBlockIndex(i));
            out += ':' + std::to_string(object->GetActiveUniformOffset(i));
            out += ':' + std::to_string(object->GetActiveUniformArrayStride(i));
            out += ':' + std::to_string(object->GetActiveUniformMatrixStride(i));
            out += ':' + std::to_string(object->GetActiveUniformIsRowMajor(i));
            out += '\n';
        }
        const Int attribCount = object->GetActiveAttributesCount();
        for (Int i = 0; i < attribCount; ++i) {
            out += object->GetActiveAttribName(static_cast<Uint>(i));
            out += ':' + std::to_string(object->GetActiveAttribType(static_cast<Uint>(i)));
            out += ':' + std::to_string(object->GetActiveAttribArraySize(static_cast<Uint>(i)));
            out += '\n';
        }
        const Int outputCount = object->GetActiveFragmentOutputCount();
        for (Int i = 0; i < outputCount; ++i) {
            out += object->GetActiveFragmentOutputName(static_cast<Uint>(i));
            out += ':' + std::to_string(object->GetFragmentOutputLocation(static_cast<Uint>(i)));
            out += ':' + std::to_string(object->GetFragmentOutputType(static_cast<Uint>(i)));
            out += '\n';
        }
        return out;
    }

    GLuint MakeShader(const GLenum type, const String& source) {
        const GLuint shader = MG_Impl::GLImpl::CreateShader(type);
        const char* text = source.c_str();
        MG_Impl::GLImpl::ShaderSource(shader, 1, &text, nullptr);
        MG_Impl::GLImpl::CompileShader(shader);
        return shader;
    }

    // One program per call, with FRESH shader objects every time - which is exactly the CTS
    // shape this cache exists for (2592 glCreateShader/glLinkProgram pairs over a handful of
    // distinct sources), and what makes the second link a genuine L1 lookup rather than a
    // reuse of an already-parsed object.
    GLuint LinkProgramFromSources(const String& vertexSource, const String& fragmentSource) {
        const GLuint vs = MakeShader(GL_VERTEX_SHADER, vertexSource);
        const GLuint fs = MakeShader(GL_FRAGMENT_SHADER, fragmentSource);
        const GLuint program = MG_Impl::GLImpl::CreateProgram();
        MG_Impl::GLImpl::AttachShader(program, vs);
        MG_Impl::GLImpl::AttachShader(program, fs);
        MG_Impl::GLImpl::LinkProgram(program);
        return program;
    }

    // The real SPIRV-Cross emission, standing in for the DirectGLES member function the L2
    // cache actually wraps. Same emitter, same options; what a unit test cannot reach is the
    // capability-gated SPIR-V pass chain around it, which needs a live ES driver to be
    // meaningful (and whose gates are covered exhaustively by the key cases instead).
    Bool EmitEssl(const Vector<Uint32>& spirv, const Uint version, String& outEssl) {
        SpvcSession session(spirv, SessionUsageBit::Transpile);
        spvc_compiler_options options;
        if (session.CreateOptions(&options) != SPVC_SUCCESS) return false;
        spvc_compiler_options_set_uint(options, SPVC_COMPILER_OPTION_GLSL_VERSION, version);
        spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_GLSL_ES, SPVC_TRUE);
        spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_GLSL_VULKAN_SEMANTICS, SPVC_FALSE);
        session.SetOptions(options);
        const char* result = nullptr;
        session.Compile(&result);
        if (!result) return false;
        outEssl = result;
        return true;
    }

    Vector<Uint32> BuildFragmentSpirv() {
        ShaderAttrib attrib{.shaderType = GL_FRAGMENT_SHADER, .sourceStr = kFragmentSource};
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

    class TranslationCacheTest : public ::testing::Test {
    protected:
        void SetUp() override { MobileGL::Initialize(); }
    };
} // namespace

// =========================================================================================
// The memo contract: eviction, collisions, lifetime
// =========================================================================================

TEST_F(TranslationCacheTest, HitReturnsTheStoredPayload) {
    BoundedTranslationCache<TestPayload> cache("test", 8, 4096);
    const TranslationCacheKey key = KeyFromText("alpha");
    EXPECT_EQ(cache.Find(key), nullptr);

    auto payload = MakeShared<TestPayload>(TestPayload{"emitted"});
    cache.Insert(key, SharedPtr<const TestPayload>(payload), PayloadBytes(*payload));

    const auto hit = cache.Find(KeyFromText("alpha"));
    ASSERT_NE(hit, nullptr);
    EXPECT_EQ(hit->text, "emitted");

    const TranslationCacheStats stats = cache.Stats();
    EXPECT_EQ(stats.hits, 1u);
    EXPECT_EQ(stats.misses, 1u);
    EXPECT_EQ(stats.inserts, 1u);
}

// The single most important property in the whole file. A 64-bit hash is a bucket
// selector; if it were ever trusted on its own, two different shaders sharing a hash
// would swap payloads and one of them would be silently miscompiled.
TEST_F(TranslationCacheTest, HashCollisionDegradesToMissNotToAWrongPayload) {
    BoundedTranslationCache<TestPayload> cache("test", 8, 4096);

    TranslationCacheKey stored;
    stored.hash = 0xdead'beef'dead'beefull;
    stored.blob = MakeShared<const String>("the real key bytes");

    TranslationCacheKey colliding;
    colliding.hash = stored.hash; // same bucket, deliberately
    colliding.blob = MakeShared<const String>("DIFFERENT key bytes");

    cache.Insert(stored, MakeShared<const TestPayload>(TestPayload{"stored payload"}), 14);

    EXPECT_EQ(cache.Find(colliding), nullptr);
    ASSERT_NE(cache.Find(stored), nullptr);
    EXPECT_EQ(cache.Find(stored)->text, "stored payload");
}

TEST_F(TranslationCacheTest, EvictionIsFifoUnderTheEntryCap) {
    BoundedTranslationCache<TestPayload> cache("test", 2, 1u << 20);
    for (const char* name : {"a", "b", "c"}) {
        auto payload = MakeShared<TestPayload>(TestPayload{name});
        cache.Insert(KeyFromText(name), SharedPtr<const TestPayload>(payload), PayloadBytes(*payload));
    }

    EXPECT_EQ(cache.EntryCount(), 2u);
    EXPECT_EQ(cache.Find(KeyFromText("a")), nullptr) << "the oldest entry should have been evicted";
    ASSERT_NE(cache.Find(KeyFromText("b")), nullptr);
    ASSERT_NE(cache.Find(KeyFromText("c")), nullptr);
    EXPECT_EQ(cache.Stats().evictions, 1u);

    // And a re-insert after the eviction works, i.e. the index and the list stayed in step.
    auto revived = MakeShared<TestPayload>(TestPayload{"a-again"});
    cache.Insert(KeyFromText("a"), SharedPtr<const TestPayload>(revived), PayloadBytes(*revived));
    const auto hit = cache.Find(KeyFromText("a"));
    ASSERT_NE(hit, nullptr);
    EXPECT_EQ(hit->text, "a-again");
}

TEST_F(TranslationCacheTest, EvictionIsFifoUnderTheByteBudget) {
    // Room for two entries by bytes, but the entry cap is generous - so the byte budget is
    // the one that has to bind.
    const SizeT keyBytes = KeyFromText("aaaa").Bytes();
    BoundedTranslationCache<TestPayload> cache("test", 64, (keyBytes + 64) * 2);
    for (const char* name : {"aaaa", "bbbb", "cccc"}) {
        auto payload = MakeShared<TestPayload>(TestPayload(String(64, name[0])));
        cache.Insert(KeyFromText(name), SharedPtr<const TestPayload>(payload), PayloadBytes(*payload));
    }
    EXPECT_EQ(cache.EntryCount(), 2u);
    EXPECT_EQ(cache.Find(KeyFromText("aaaa")), nullptr);
    EXPECT_NE(cache.Find(KeyFromText("cccc")), nullptr);
    EXPECT_LE(cache.StoredBytes(), (keyBytes + 64) * 2);
}

TEST_F(TranslationCacheTest, AnEntryLargerThanTheWholeBudgetIsNotCached) {
    BoundedTranslationCache<TestPayload> cache("test", 64, 128);
    auto payload = MakeShared<TestPayload>(TestPayload(String(4096, 'x')));
    cache.Insert(KeyFromText("huge"), SharedPtr<const TestPayload>(payload), PayloadBytes(*payload));
    EXPECT_EQ(cache.EntryCount(), 0u);
    EXPECT_EQ(cache.Stats().rejectedOversize, 1u);
    EXPECT_EQ(cache.Find(KeyFromText("huge")), nullptr);
}

// A hit hands out shared ownership, so a reader still holding a payload when the entry is
// evicted keeps reading valid memory. This is what makes the memo safe once compiles run
// on pool workers.
TEST_F(TranslationCacheTest, APayloadOutlivesTheEvictionOfItsEntry) {
    BoundedTranslationCache<TestPayload> cache("test", 1, 1u << 20);
    auto first = MakeShared<TestPayload>(TestPayload{"first"});
    cache.Insert(KeyFromText("first"), SharedPtr<const TestPayload>(first), PayloadBytes(*first));
    const auto held = cache.Find(KeyFromText("first"));
    ASSERT_NE(held, nullptr);

    auto second = MakeShared<TestPayload>(TestPayload{"second"});
    cache.Insert(KeyFromText("second"), SharedPtr<const TestPayload>(second), PayloadBytes(*second));
    EXPECT_EQ(cache.Find(KeyFromText("first")), nullptr);
    EXPECT_EQ(held->text, "first"); // still readable
}

// ska::flat_hash_map iterates in insertion/capacity order, so a key builder that walked a
// map directly would produce different bytes for the same map depending on how it was
// filled - a pure loss (spurious misses), and one that is invisible without this case.
TEST_F(TranslationCacheTest, NameMapsSerializeCanonically) {
    UnorderedMap<String, Uint> forward;
    forward.emplace("aPos", 0u);
    forward.emplace("aNormal", 1u);
    forward.emplace("aUv", 2u);

    UnorderedMap<String, Uint> reverse;
    reverse.emplace("aUv", 2u);
    reverse.emplace("aNormal", 1u);
    reverse.emplace("aPos", 0u);

    TranslationKeyBuilder a;
    a.NameMap(forward);
    TranslationKeyBuilder b;
    b.NameMap(reverse);
    EXPECT_EQ(a.Blob(), b.Blob());

    // ... and a value change still moves it.
    reverse["aPos"] = 7u;
    TranslationKeyBuilder c;
    c.NameMap(reverse);
    EXPECT_NE(a.Blob(), c.Blob());
}

// Length-prefixing: "ab" + "c" must not serialize to the same bytes as "a" + "bc".
TEST_F(TranslationCacheTest, TextAppendsCannotRunIntoEachOther) {
    TranslationKeyBuilder a;
    a.Text("ab");
    a.Text("c");
    TranslationKeyBuilder b;
    b.Text("a");
    b.Text("bc");
    EXPECT_NE(a.Blob(), b.Blob());
}

TEST_F(TranslationCacheTest, L1AndL2KeysNeverAlias) {
    const Vector<Uint32> spirv{1u, 2u, 3u};
    const TranslationCacheKey l2 = BuildEsslTranslationKey(BaselineEsslInputs(spirv));
    const TranslationCacheKey l1 =
        BuildSpirvTranslationKey(BaselineSpirvInputs({{GL_FRAGMENT_SHADER, "source"}}));
    EXPECT_FALSE(l1 == l2);
}

// =========================================================================================
// L1 key composition - one case per input that can change the produced SPIR-V
// =========================================================================================

TEST_F(TranslationCacheTest, L1KeyMovesWithEveryInputThatMovesTheSpirv) {
    const String vs = kVertexSource;
    const String fs = kFragmentSource;
    const Vector<SpirvTranslationKeyInputs::Stage> baseStages{
        {GL_VERTEX_SHADER, vs}, {GL_FRAGMENT_SHADER, fs}};

    const UnorderedMap<String, Uint> attribs{{"aPos", 3u}};
    const UnorderedMap<String, Uint> fragOut{{"fragColor", 1u}};
    const UnorderedMap<String, Uint> fragIndex{{"fragColor", 1u}};
    const UnorderedMap<String, Uint> opaque{{"uTex", 5u}};
    const Vector<String> xfbVaryings{"vPos", "gl_NextBuffer", "vUv"};
    const Vector<String> xfbVaryingsReordered{"vUv", "gl_NextBuffer", "vPos"};

    const SpirvTranslationKeyInputs base = BaselineSpirvInputs(baseStages);
    const TranslationCacheKey baseKey = BuildSpirvTranslationKey(base);

    // Identical inputs -> identical key. Everything below is measured against this.
    EXPECT_TRUE(BuildSpirvTranslationKey(BaselineSpirvInputs(baseStages)) == baseKey);

    Vector<Pair<const char*, TranslationCacheKey>> variants;

    {   // the environment fingerprint (glslang resource limits, backend identity,
        // advertised extension set, compute limits)
        SpirvTranslationKeyInputs v = base;
        v.frontendFingerprint ^= 1ull;
        variants.emplace_back("frontendFingerprint", BuildSpirvTranslationKey(v));
    }
    {   // a stage's source text
        const String otherFs = SwizzleLikeFragment("i");
        SpirvTranslationKeyInputs v = BaselineSpirvInputs({{GL_VERTEX_SHADER, vs},
                                                           {GL_FRAGMENT_SHADER, otherFs}});
        variants.emplace_back("stage source", BuildSpirvTranslationKey(v));
    }
    {   // a stage's TYPE, with the text unchanged
        SpirvTranslationKeyInputs v = BaselineSpirvInputs({{GL_VERTEX_SHADER, vs},
                                                           {GL_COMPUTE_SHADER, fs}});
        variants.emplace_back("stage type", BuildSpirvTranslationKey(v));
    }
    {   // the SET of stages - mapIO resolves a fragment stage's Locations against the
        // vertex stage's outputs, which is why this key is per PROGRAM and not per stage
        SpirvTranslationKeyInputs v = BaselineSpirvInputs({{GL_FRAGMENT_SHADER, fs}});
        variants.emplace_back("stage set", BuildSpirvTranslationKey(v));
    }
    {   // stage ORDER
        SpirvTranslationKeyInputs v = BaselineSpirvInputs({{GL_FRAGMENT_SHADER, fs},
                                                           {GL_VERTEX_SHADER, vs}});
        variants.emplace_back("stage order", BuildSpirvTranslationKey(v));
    }
    {   // glBindAttribLocation
        SpirvTranslationKeyInputs v = base;
        v.explicitVertexInLocations = &attribs;
        variants.emplace_back("explicitVertexInLocations", BuildSpirvTranslationKey(v));
    }
    {   // glBindFragDataLocation
        SpirvTranslationKeyInputs v = base;
        v.explicitFragmentOutLocations = &fragOut;
        variants.emplace_back("explicitFragmentOutLocations", BuildSpirvTranslationKey(v));
    }
    {   // glBindFragDataLocationIndexed
        SpirvTranslationKeyInputs v = base;
        v.explicitFragmentOutIndices = &fragIndex;
        variants.emplace_back("explicitFragmentOutIndices", BuildSpirvTranslationKey(v));
    }
    {   // the merged layout(binding = N) opaque units
        SpirvTranslationKeyInputs v = base;
        v.explicitOpaqueUniformBindings = &opaque;
        variants.emplace_back("explicitOpaqueUniformBindings", BuildSpirvTranslationKey(v));
    }
    {   // ShaderCompileBits (0 on both production parse paths; keyed so a future value
        // cannot alias a module parsed without it)
        SpirvTranslationKeyInputs v = base;
        v.shaderCompileFlags = 1u;
        variants.emplace_back("shaderCompileFlags", BuildSpirvTranslationKey(v));
    }
    {   // MOBILEGL_ENABLE_SPIRV_VALIDATION
        SpirvTranslationKeyInputs v = base;
        v.enableSpirvValidation = true;
        variants.emplace_back("enableSpirvValidation", BuildSpirvTranslationKey(v));
    }
    // ---- inputs the WIDENED payload pulled into the key ----
    // They cannot move a word of the generated SPIR-V, but they do shape the reflection the
    // payload now carries, so they have to split the key. This is the group that would go
    // stale first if the payload ever grew again without the key following it.
    {   // glTransformFeedbackVaryings: shapes xfbVaryings / xfbStrides / gsStripTriangles
        SpirvTranslationKeyInputs v = base;
        v.requestedXfbVaryings = &xfbVaryings;
        variants.emplace_back("requestedXfbVaryings", BuildSpirvTranslationKey(v));
    }
    {   // ... and its ORDER, which gl_NextBuffer / gl_SkipComponentsN make load-bearing
        SpirvTranslationKeyInputs v = base;
        v.requestedXfbVaryings = &xfbVaryingsReordered;
        variants.emplace_back("requestedXfbVaryings order", BuildSpirvTranslationKey(v));
    }
    {   // GL_INTERLEAVED_ATTRIBS vs GL_SEPARATE_ATTRIBS
        SpirvTranslationKeyInputs v = base;
        v.xfbBufferMode = GL_SEPARATE_ATTRIBS;
        variants.emplace_back("xfbBufferMode", BuildSpirvTranslationKey(v));
    }
    {   // GL_MAX_DRAW_BUFFERS: decides whether the link is REJECTED at all
        SpirvTranslationKeyInputs v = base;
        v.maxFragmentOutputColorNumber = 4;
        variants.emplace_back("maxFragmentOutputColorNumber", BuildSpirvTranslationKey(v));
    }

    for (const auto& [name, key] : variants) {
        EXPECT_FALSE(key == baseKey) << "moving " << name << " did not move the L1 key";
    }
    // Pairwise distinct too: two different inputs must not collapse onto one key.
    for (SizeT i = 0; i < variants.size(); ++i) {
        for (SizeT j = i + 1; j < variants.size(); ++j) {
            EXPECT_FALSE(variants[i].second == variants[j].second)
                << variants[i].first << " and " << variants[j].first << " produce the same L1 key";
        }
    }
}

// =========================================================================================
// L1 backend-agnosticism: the environment inputs that were REMOVED from the key
// =========================================================================================

namespace {
    // Two environments that differ in every way that only steers a BACKEND, and in no way
    // that reaches glslang.
    Pair<CompileEnv, CompileEnv> BackendOnlyDifferentEnvs() {
        CompileEnv a;
        CompileEnv b;
        // (1) backend identity - both HAVE a backend, they are just different ones
        a.backend = BackendType::DirectGLES;
        b.backend = BackendType::DirectVulkan;
        // (2) the advertised extension vector, including the fp64 flag's own extension
        a.advertisedExtensions = {E_GL_ARB_gpu_shader_fp64, E_GL_KHR_debug};
        b.advertisedExtensions = {};
        // (3) the compute limits (ValidateComputeLocalSizeLimits only)
        a.maxComputeWorkGroupSize[0] = 1024;
        a.maxComputeWorkGroupSize[1] = 1024;
        a.maxComputeWorkGroupSize[2] = 64;
        a.maxComputeWorkGroupInvocations = 128;
        b.maxComputeWorkGroupSize[0] = 2048;
        b.maxComputeWorkGroupSize[1] = 2048;
        b.maxComputeWorkGroupSize[2] = 1024;
        b.maxComputeWorkGroupInvocations = 2048;
        // (4) a spread of DynamicBackendParameters fields the front end never reads
        a.params.MaxColorTextureSamples = 1;
        b.params.MaxColorTextureSamples = 8;
        a.params.MaxTextureSize = 4096;
        b.params.MaxTextureSize = 16384;
        a.params.MaxViewports = 1;
        b.params.MaxViewports = 16;
        a.params.MaxUniformBufferBindings = 24;
        b.params.MaxUniformBufferBindings = 84;
        a.params.MaxTextureImageUnits = 16;
        b.params.MaxTextureImageUnits = 32;
        return {a, b};
    }
} // namespace

// THE case that pins L1's backend-agnosticism. Everything moved here is something that
// only steers a backend transpile, and L2 already keys on the ones that matter there.
// The old whole-environment fingerprint moves; the front-end one must not.
TEST_F(TranslationCacheTest, TheFrontendFingerprintIgnoresBackendOnlyDifferences) {
    const auto [a, b] = BackendOnlyDifferentEnvs();

    EXPECT_NE(ComputeCompileEnvFingerprint(a), ComputeCompileEnvFingerprint(b))
        << "the whole-environment fingerprint is supposed to notice these; if it does not, "
           "this case is no longer testing anything";
    EXPECT_EQ(ComputeFrontendCompileEnvFingerprint(a), ComputeFrontendCompileEnvFingerprint(b))
        << "a backend-only difference leaked into the front-end fingerprint";
}

// ... and the same thing one level up: the two environments must produce ONE L1 entry.
TEST_F(TranslationCacheTest, TwoBackendsCompilingTheSameGlslShareOneL1Entry) {
    const auto [a, b] = BackendOnlyDifferentEnvs();
    const String vs = kVertexSource;
    const String fs = kFragmentSource;
    const Vector<SpirvTranslationKeyInputs::Stage> stages{{GL_VERTEX_SHADER, vs},
                                                          {GL_FRAGMENT_SHADER, fs}};

    SpirvTranslationKeyInputs onA = BaselineSpirvInputs(stages);
    onA.frontendFingerprint = ComputeFrontendCompileEnvFingerprint(a);
    SpirvTranslationKeyInputs onB = BaselineSpirvInputs(stages);
    onB.frontendFingerprint = ComputeFrontendCompileEnvFingerprint(b);

    EXPECT_TRUE(BuildSpirvTranslationKey(onA) == BuildSpirvTranslationKey(onB));
}

// The other direction, one case per input that was KEPT. Each is a limit the front end
// really consumes - the seven BuildTBuiltInResource copies into TBuiltInResource, plus the
// two inputs to the reflection vertex-attrib limit - so each must still split the key.
TEST_F(TranslationCacheTest, TheFrontendFingerprintMovesWithEveryFrontendLimit) {
    const CompileEnv base;
    const Uint64 baseline = ComputeFrontendCompileEnvFingerprint(base);

    const Vector<Pair<const char*, std::function<void(CompileEnv&)>>> mutations{
        {"params.MaxImageUnits", [](CompileEnv& e) { e.params.MaxImageUnits += 1; }},
        {"params.MaxDrawBuffers", [](CompileEnv& e) { e.params.MaxDrawBuffers += 1; }},
        {"params.MaxVertexImageUniforms", [](CompileEnv& e) { e.params.MaxVertexImageUniforms += 1; }},
        {"params.MaxGeometryImageUniforms", [](CompileEnv& e) { e.params.MaxGeometryImageUniforms += 1; }},
        {"params.MaxFragmentImageUniforms", [](CompileEnv& e) { e.params.MaxFragmentImageUniforms += 1; }},
        {"params.MaxComputeImageUniforms", [](CompileEnv& e) { e.params.MaxComputeImageUniforms += 1; }},
        {"params.MaxCombinedImageUniforms", [](CompileEnv& e) { e.params.MaxCombinedImageUniforms += 1; }},
        {"params.MaxVertexAttribs", [](CompileEnv& e) { e.params.MaxVertexAttribs += 1; }},
        // HasBackend(): with no backend the reflection attrib limit falls back to the
        // storage capacity rather than the driver's number, so the bit is load-bearing.
        {"HasBackend", [](CompileEnv& e) { e.backend = BackendType::DirectGLES; }},
    };

    Vector<Uint64> seen{baseline};
    for (const auto& [name, mutate] : mutations) {
        CompileEnv env = base;
        mutate(env);
        const Uint64 moved = ComputeFrontendCompileEnvFingerprint(env);
        EXPECT_NE(moved, baseline) << "moving " << name << " did not move the front-end fingerprint";
        for (const Uint64 previous : seen) {
            EXPECT_NE(moved, previous) << name << " collides with an earlier front-end limit";
        }
        seen.push_back(moved);
    }
}

// =========================================================================================
// L1 end to end, through the real GL entry points
// =========================================================================================

// The headline case: the second program with byte-identical sources reuses the first
// program's modules instead of running GlslangToSpv and the 11-pass sanitize chain again,
// and the modules it gets are the same bytes.
TEST_F(TranslationCacheTest, L1MemoizesASecondProgramWithIdenticalSources) {
    const SyncCompileScope sync;
    const CacheModeScope cacheOn(true);

    const String fs = SwizzleLikeFragment("");
    const TranslationCacheStats before = MG_State::GLState::GetProgramTranslationCache().Stats();

    const GLuint first = LinkProgramFromSources(kVertexSource, fs);
    const TranslationCacheStats afterFirst = MG_State::GLState::GetProgramTranslationCache().Stats();
    const GLuint second = LinkProgramFromSources(kVertexSource, fs);
    const TranslationCacheStats afterSecond = MG_State::GLState::GetProgramTranslationCache().Stats();

    GLint firstStatus = GL_FALSE;
    GLint secondStatus = GL_FALSE;
    MG_Impl::GLImpl::GetProgramiv(first, GL_LINK_STATUS, &firstStatus);
    MG_Impl::GLImpl::GetProgramiv(second, GL_LINK_STATUS, &secondStatus);
    ASSERT_EQ(firstStatus, GL_TRUE);
    ASSERT_EQ(secondStatus, GL_TRUE);

    EXPECT_EQ(afterFirst.misses - before.misses, 1u) << "the first link must be a miss";
    EXPECT_EQ(afterFirst.hits - before.hits, 0u);
    EXPECT_EQ(afterSecond.hits - afterFirst.hits, 1u) << "the second link must be a hit";
    EXPECT_EQ(afterSecond.misses - afterFirst.misses, 0u);

    const Vector<Uint64> firstDigest = ProgramSpirvDigest(first);
    const Vector<Uint64> secondDigest = ProgramSpirvDigest(second);
    ASSERT_EQ(firstDigest.size(), 2u);
    EXPECT_EQ(firstDigest, secondDigest);

    // The payload is the WHOLE front end, so the reflection has to survive it too - the
    // program served from the memo never built a TProgram to answer these from.
    EXPECT_EQ(ReflectionFingerprint(first), ReflectionFingerprint(second));
    EXPECT_FALSE(ReflectionFingerprint(second).empty());
}

// A hit publishes a program that never had a glslang::TProgram at all. Everything GL can ask
// about it has to come out of the payload; if any accessor still needed the parse this would
// answer differently from the program that was parsed.
TEST_F(TranslationCacheTest, AProgramServedFromTheMemoAnswersTheWholeQuerySurface) {
    const SyncCompileScope sync;
    const CacheModeScope cacheOn(true);
    const String fs = SwizzleLikeFragment("");

    const GLuint parsed = LinkProgramFromSources(kVertexSource, fs);
    const TranslationCacheStats afterFirst = MG_State::GLState::GetProgramTranslationCache().Stats();
    const GLuint fromMemo = LinkProgramFromSources(kVertexSource, fs);
    const TranslationCacheStats afterSecond = MG_State::GLState::GetProgramTranslationCache().Stats();
    ASSERT_EQ(afterSecond.hits - afterFirst.hits, 1u) << "the second link was not a hit";

    const auto& parsedObject = MG_State::pGLContext->GetProgramObject(parsed);
    const auto& memoObject = MG_State::pGLContext->GetProgramObject(fromMemo);
    ASSERT_NE(parsedObject, nullptr);
    ASSERT_NE(memoObject, nullptr);

    EXPECT_EQ(ReflectionFingerprint(parsed), ReflectionFingerprint(fromMemo));
    EXPECT_EQ(parsedObject->GetUniformCount(), memoObject->GetUniformCount());
    EXPECT_EQ(parsedObject->GetActiveAttributesCount(), memoObject->GetActiveAttributesCount());
    EXPECT_EQ(parsedObject->GetActiveFragmentOutputCount(), memoObject->GetActiveFragmentOutputCount());
    EXPECT_EQ(parsedObject->GetActiveUniformBlocksCount(), memoObject->GetActiveUniformBlocksCount());
    // The uniform shadow (phase B's half of the payload) has to arrive as well.
    for (Uint location = 0; location <= parsedObject->GetMaxUniformLocation(); ++location) {
        EXPECT_EQ(parsedObject->GetUniformOffset(location), memoObject->GetUniformOffset(location))
            << "uniform offset at location " << location;
    }
}

// The modules a hit hands out must be the modules a from-scratch translation would have
// produced. Without this the case above would still pass if the cache returned garbage.
TEST_F(TranslationCacheTest, L1HitsAgreeWithACacheDisabledTranslation) {
    const SyncCompileScope sync;
    const String fs = SwizzleLikeFragment("u");

    Vector<Uint64> uncached;
    {
        const CacheModeScope cacheOff(false);
        uncached = ProgramSpirvDigest(LinkProgramFromSources(kVertexSource, fs));
    }
    ASSERT_EQ(uncached.size(), 2u);

    const CacheModeScope cacheOn(true);
    const Vector<Uint64> primed = ProgramSpirvDigest(LinkProgramFromSources(kVertexSource, fs));
    const Vector<Uint64> hit = ProgramSpirvDigest(LinkProgramFromSources(kVertexSource, fs));
    EXPECT_EQ(primed, uncached);
    EXPECT_EQ(hit, uncached);
}

TEST_F(TranslationCacheTest, L1DoesNotMemoizeAcrossDifferentSources) {
    const SyncCompileScope sync;
    const CacheModeScope cacheOn(true);

    // Prime with one, then link a different one: a miss, not a hit.
    (void)LinkProgramFromSources(kVertexSource, SwizzleLikeFragment(""));
    const TranslationCacheStats before = MG_State::GLState::GetProgramTranslationCache().Stats();
    (void)LinkProgramFromSources(kVertexSource, SwizzleLikeFragment("i"));
    const TranslationCacheStats after = MG_State::GLState::GetProgramTranslationCache().Stats();

    EXPECT_EQ(after.hits - before.hits, 0u);
    EXPECT_EQ(after.misses - before.misses, 1u);
}

// The escape hatch. MOBILEGL_SHADER_CACHE falsy must make every link translate again -
// no lookup at all, so a field miscompile can be bisected against the feature in one run.
TEST_F(TranslationCacheTest, TheEscapeHatchDisablesL1Entirely) {
    const SyncCompileScope sync;
    const String fs = SwizzleLikeFragment("");

    {   // prime the cache with the switch ON, so a later hit would be available
        const CacheModeScope cacheOn(true);
        (void)LinkProgramFromSources(kVertexSource, fs);
    }

    const CacheModeScope cacheOff(false);
    const TranslationCacheStats before = MG_State::GLState::GetProgramTranslationCache().Stats();
    const GLuint program = LinkProgramFromSources(kVertexSource, fs);
    const TranslationCacheStats after = MG_State::GLState::GetProgramTranslationCache().Stats();

    GLint status = GL_FALSE;
    MG_Impl::GLImpl::GetProgramiv(program, GL_LINK_STATUS, &status);
    EXPECT_EQ(status, GL_TRUE) << "the program must still link with the cache off";
    EXPECT_EQ(after.hits, before.hits) << "no lookup may happen with the cache disabled";
    EXPECT_EQ(after.misses, before.misses);
    EXPECT_EQ(after.inserts, before.inserts);
}

// =========================================================================================
// L2 key composition - one case per gate the DirectGLES pass chain arms
// =========================================================================================

TEST_F(TranslationCacheTest, L2KeyMovesWithEveryGateThatSteersTheEsslChain) {
    const Vector<Uint32> spirv{0x07230203u, 0x00010300u, 0u, 32u, 0u};
    const Vector<Uint32> otherSpirv{0x07230203u, 0x00010300u, 0u, 33u, 0u};
    const std::set<String> xfbBlocks{"StageData"};
    const UnorderedMap<String, Uint> imageFormats{{"gImage", 0x8236u /*GL_R32UI*/}};
    const UnorderedMap<String, Int> storageBindings{{"Data", 3}};

    const EsslTranslationKeyInputs base = BaselineEsslInputs(spirv);
    const TranslationCacheKey baseKey = BuildEsslTranslationKey(base);
    EXPECT_TRUE(BuildEsslTranslationKey(BaselineEsslInputs(spirv)) == baseKey);

    Vector<Pair<const char*, TranslationCacheKey>> variants;

    {   // the module itself
        EsslTranslationKeyInputs v = base;
        v.spirv = &otherSpirv;
        variants.emplace_back("spirv", BuildEsslTranslationKey(v));
    }
    {   // stage: gates LowerDrawParametersForEssl and SplitArrayVertexInputsForEssl (vertex)
        // and LegalizeFragmentOutputIndexingForEssl (fragment)
        EsslTranslationKeyInputs v = base;
        v.shaderType = GL_VERTEX_SHADER;
        variants.emplace_back("shaderType", BuildEsslTranslationKey(v));
    }
    {   // arms LowerViewportIndexForEssl
        EsslTranslationKeyInputs v = base;
        v.supportsViewportArray = true;
        variants.emplace_back("supportsViewportArray", BuildEsslTranslationKey(v));
    }
    {   // arms EmulateNoPerspectiveForEssl
        EsslTranslationKeyInputs v = base;
        v.supportsNoperspectiveInterpolation = true;
        variants.emplace_back("supportsNoperspectiveInterpolation", BuildEsslTranslationKey(v));
    }
    {   // arms AND parameterizes ClampMultisampleFetchesForEssl
        EsslTranslationKeyInputs v = base;
        v.maxColorTextureSamples = 1;
        variants.emplace_back("maxColorTextureSamples", BuildEsslTranslationKey(v));
    }
    {
        EsslTranslationKeyInputs v = base;
        v.maxIntegerSamples = 4;
        variants.emplace_back("maxIntegerSamples", BuildEsslTranslationKey(v));
    }
    {
        EsslTranslationKeyInputs v = base;
        v.maxDepthTextureSamples = 1;
        variants.emplace_back("maxDepthTextureSamples", BuildEsslTranslationKey(v));
    }
    {   // the ceiling the three above are compared against
        EsslTranslationKeyInputs v = base;
        v.advertisedMaxSamples = 8;
        variants.emplace_back("advertisedMaxSamples", BuildEsslTranslationKey(v));
    }
    {   // the argument to FlattenXfbInterfaceBlocksForEssl
        EsslTranslationKeyInputs v = base;
        v.xfbCaptureBlockNames = &xfbBlocks;
        variants.emplace_back("xfbCaptureBlockNames", BuildEsslTranslationKey(v));
    }
    {   // the argument to BakeImageFormatsForEssl - live glBindImageTexture state
        EsslTranslationKeyInputs v = base;
        v.glFormatByUniformName = &imageFormats;
        variants.emplace_back("glFormatByUniformName", BuildEsslTranslationKey(v));
    }
    {   // SpvcSession::SetShaderStorageBlockBinding
        EsslTranslationKeyInputs v = base;
        v.storageBlockBindingOverrides = &storageBindings;
        variants.emplace_back("storageBlockBindingOverrides", BuildEsslTranslationKey(v));
    }
    {   // SPVC_COMPILER_OPTION_GLSL_VERSION (ResolveBackendEsslVersion)
        EsslTranslationKeyInputs v = base;
        v.esslVersion = 300;
        variants.emplace_back("esslVersion", BuildEsslTranslationKey(v));
    }
    {
        EsslTranslationKeyInputs v = base;
        v.enableSpirvValidation = true;
        variants.emplace_back("enableSpirvValidation", BuildEsslTranslationKey(v));
    }

    for (const auto& [name, key] : variants) {
        EXPECT_FALSE(key == baseKey) << "moving " << name << " did not move the L2 key";
    }
    for (SizeT i = 0; i < variants.size(); ++i) {
        for (SizeT j = i + 1; j < variants.size(); ++j) {
            EXPECT_FALSE(variants[i].second == variants[j].second)
                << variants[i].first << " and " << variants[j].first << " produce the same L2 key";
        }
    }
}

// The value SIDE of L2: the payload has to carry the flattened-block report, not just the
// text. A payload that dropped it would silently un-rename every transform-feedback
// capture on a hit.
TEST_F(TranslationCacheTest, L2PayloadCarriesTheFlattenedXfbBlockReport) {
    BoundedTranslationCache<EsslTranslationResult> cache("test", 8, 1u << 20);
    const Vector<Uint32> spirv{1u, 2u, 3u};
    const TranslationCacheKey key = BuildEsslTranslationKey(BaselineEsslInputs(spirv));

    auto payload = MakeShared<EsslTranslationResult>();
    payload->essl = "#version 320 es\nvoid main() {}\n";
    payload->flattenedXfbBlockNames = {"StageData", "OtherBlock"};
    cache.Insert(key, EsslTranslationResultPtr(payload), EsslTranslationResultBytes(*payload));

    const auto hit = cache.Find(BuildEsslTranslationKey(BaselineEsslInputs(spirv)));
    ASSERT_NE(hit, nullptr);
    EXPECT_EQ(hit->essl, payload->essl);
    EXPECT_EQ(hit->flattenedXfbBlockNames, payload->flattenedXfbBlockNames);
}

// The real emitter behind the real key: two lookups over the same module and the same
// capability snapshot run SPIRV-Cross once and return the same text; moving one capability
// bit runs it again.
TEST_F(TranslationCacheTest, L2RunsTheEmitterOncePerDistinctKey) {
    const Vector<Uint32> spirv = BuildFragmentSpirv();
    ASSERT_FALSE(spirv.empty());

    BoundedTranslationCache<EsslTranslationResult> cache("test", 8, 4u << 20);
    Int emitCount = 0;

    const auto translate = [&](const EsslTranslationKeyInputs& inputs) -> String {
        const TranslationCacheKey key = BuildEsslTranslationKey(inputs);
        if (const auto hit = cache.Find(key)) return hit->essl;
        auto payload = MakeShared<EsslTranslationResult>();
        EXPECT_TRUE(EmitEssl(*inputs.spirv, inputs.esslVersion, payload->essl));
        ++emitCount;
        cache.Insert(key, EsslTranslationResultPtr(payload), EsslTranslationResultBytes(*payload));
        return payload->essl;
    };

    EsslTranslationKeyInputs inputs = BaselineEsslInputs(spirv);
    const String first = translate(inputs);
    const String second = translate(inputs);
    EXPECT_FALSE(first.empty());
    EXPECT_EQ(first, second);
    EXPECT_EQ(emitCount, 1) << "the second lookup must not have reached SPIRV-Cross";

    // A capability bit moves -> the emitter runs again. (esslVersion is the one this unit
    // test can observe in the OUTPUT as well as in the key.)
    inputs.esslVersion = 300;
    const String downlevel = translate(inputs);
    EXPECT_EQ(emitCount, 2);
    EXPECT_NE(downlevel, first);

    // ... and a gate that only steers the SPIR-V pass chain still moves the key, so the
    // emitter runs again even though this stand-in ignores the bit.
    inputs = BaselineEsslInputs(spirv);
    inputs.supportsViewportArray = true;
    (void)translate(inputs);
    EXPECT_EQ(emitCount, 3);
}

// =========================================================================================
// Thread safety
// =========================================================================================

// Several threads racing Find/compute/Insert over OVERLAPPING keys. Two workers that miss
// on the same key both compute it - deliberately, because waiting on each other inside a
// job body is what deadlocks ShaderCompilePool - so the property under test is not "the
// work happened once" but "every payload handed out equals the single-threaded answer".
TEST_F(TranslationCacheTest, ConcurrentLookupsOverOverlappingKeysAgreeWithTheSerialAnswer) {
    constexpr Int kDistinctKeys = 24;
    constexpr Int kThreads = 8;
    constexpr Int kRoundsPerThread = 200;

    const auto expensive = [](const Int index) {
        return String("payload-") + std::to_string(index) + String(64, static_cast<char>('a' + index % 26));
    };

    BoundedTranslationCache<TestPayload> cache("test", kDistinctKeys, 8u << 20);
    std::atomic<Int> mismatches{0};
    std::atomic<Int> nulls{0};

    Vector<std::thread> threads;
    threads.reserve(kThreads);
    for (Int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            for (Int round = 0; round < kRoundsPerThread; ++round) {
                const Int index = (round * 7 + t * 3) % kDistinctKeys;
                const TranslationCacheKey key = KeyFromText("key-" + std::to_string(index));
                SharedPtr<const TestPayload> value = cache.Find(key);
                if (!value) {
                    auto fresh = MakeShared<TestPayload>(TestPayload{expensive(index)});
                    cache.Insert(key, SharedPtr<const TestPayload>(fresh), PayloadBytes(*fresh));
                    value = fresh;
                }
                if (!value) {
                    nulls.fetch_add(1, std::memory_order_relaxed);
                } else if (value->text != expensive(index)) {
                    mismatches.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (std::thread& thread : threads) thread.join();

    EXPECT_EQ(mismatches.load(), 0) << "a worker was handed a payload belonging to another key";
    EXPECT_EQ(nulls.load(), 0);
    EXPECT_LE(cache.EntryCount(), static_cast<SizeT>(kDistinctKeys));
    const TranslationCacheStats stats = cache.Stats();
    EXPECT_EQ(stats.hits + stats.misses, static_cast<Uint64>(kThreads) * kRoundsPerThread);
}

// The same race with eviction turned up so hard that entries are constantly being erased
// under the readers - the shape that would catch a Find() that handed back a pointer into
// the entry list instead of shared ownership.
TEST_F(TranslationCacheTest, ConcurrentLookupsStaySafeWhileEvictionChurns) {
    constexpr Int kDistinctKeys = 32;
    constexpr Int kThreads = 8;
    constexpr Int kRoundsPerThread = 400;

    BoundedTranslationCache<TestPayload> cache("test", 4, 1u << 20); // 4 slots for 32 keys
    std::atomic<Int> mismatches{0};

    Vector<std::thread> threads;
    threads.reserve(kThreads);
    for (Int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            for (Int round = 0; round < kRoundsPerThread; ++round) {
                const Int index = (round + t) % kDistinctKeys;
                const String expected = "payload-" + std::to_string(index);
                const TranslationCacheKey key = KeyFromText("key-" + std::to_string(index));
                SharedPtr<const TestPayload> value = cache.Find(key);
                if (!value) {
                    auto fresh = MakeShared<TestPayload>(TestPayload{expected});
                    cache.Insert(key, SharedPtr<const TestPayload>(fresh), PayloadBytes(*fresh));
                    value = fresh;
                }
                // Held across further cache traffic on purpose: the payload must stay valid
                // even after its entry has been evicted by another thread.
                std::this_thread::yield();
                if (value->text != expected) mismatches.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (std::thread& thread : threads) thread.join();

    EXPECT_EQ(mismatches.load(), 0);
    EXPECT_LE(cache.EntryCount(), 4u);
}

// And the production shape: many links of a handful of distinct sources across the real
// compile pool, with L1 live. Every program built from the same sources must end up with
// the same SPIR-V, whichever worker won the race to translate it.
TEST_F(TranslationCacheTest, ConcurrentLinksOfSharedSourcesProduceIdenticalSpirv) {
    const CacheModeScope cacheOn(true);
    constexpr Int kVariants = 3;
    constexpr Int kProgramsPerVariant = 8;

    Vector<String> sources;
    for (Int v = 0; v < kVariants; ++v) sources.push_back(SwizzleLikeFragment(v == 0 ? "" : (v == 1 ? "i" : "u")));

    Vector<GLuint> programs;
    Vector<Int> variantOf;
    for (Int round = 0; round < kProgramsPerVariant; ++round) {
        for (Int v = 0; v < kVariants; ++v) {
            programs.push_back(LinkProgramFromSources(kVertexSource, sources[v]));
            variantOf.push_back(v);
        }
    }

    Vector<Vector<Uint64>> expected(kVariants);
    for (SizeT i = 0; i < programs.size(); ++i) {
        GLint status = GL_FALSE;
        MG_Impl::GLImpl::GetProgramiv(programs[i], GL_LINK_STATUS, &status);
        ASSERT_EQ(status, GL_TRUE);
        const Vector<Uint64> digest = ProgramSpirvDigest(programs[i]);
        ASSERT_EQ(digest.size(), 2u);
        const Int v = variantOf[i];
        if (expected[v].empty()) {
            expected[v] = digest;
        } else {
            EXPECT_EQ(digest, expected[v]) << "variant " << v << " program " << i;
        }
    }
    // The three variants must not have collapsed onto one another.
    EXPECT_NE(expected[0], expected[1]);
    EXPECT_NE(expected[1], expected[2]);
}
