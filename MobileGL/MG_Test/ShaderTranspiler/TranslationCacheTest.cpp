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
        inputs.viewportIndexLoweringArmed = false;
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
    // NOT the merged layout(binding = N) opaque units, which used to be a variant here: that
    // map is an OUTPUT of mapIO (TMglGlslIoResolver writes it and never reads it), so it is a
    // pure function of the stage sources this key already carries in full. It was dropped from
    // SpirvTranslationKeyInputs with the glslang-capture migration; kKeyLayoutVersion moved to
    // 4 so no blob written under the old shape can be honoured.
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
    {   // CompileEnv::ConsumesFloat64Natively(): the fp64 tail of SanitizeAndOptimizeBinary is
        // skipped under it, so the SAME GLSL yields modules with real doubles under one answer
        // and demoted, storage-block-flattened ones under the other. The one backend capability
        // bit in this key, and the only one allowed in without changing what glslang produces.
        SpirvTranslationKeyInputs v = base;
        v.nativeFloat64 = true;
        variants.emplace_back("nativeFloat64", BuildSpirvTranslationKey(v));
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
        // (3) the compute INVOCATION limit, and deliberately not the work-group size or
        // count any more. Those two used to sit here on the grounds that
        // ValidateComputeLocalSizeLimits was their only consumer; wave3 (cb155c5b) made
        // BuildTBuiltInResource read them, and glslang expands both into built-in constants
        // (gl_MaxComputeWorkGroupSize / gl_MaxComputeWorkGroupCount), so they are now
        // front-end inputs and belong in TheFrontendFingerprintMovesWithEveryFrontendLimit
        // instead - which is where they moved. The invocation limit is the one that really
        // still stops at the pre-parse gate: glslang has no built-in constant for it and
        // BuildTBuiltInResource does not read it.
        a.maxComputeWorkGroupInvocations = 128;
        b.maxComputeWorkGroupInvocations = 2048;
        // (4) a spread of DynamicBackendParameters fields the front end never reads.
        // MaxTextureImageUnits used to be here and is NOT any more: the GL 4.6 API-surface wave
        // made BuildTBuiltInResource read it (gl_MaxTextureImageUnits expands from it), so it
        // moved to TheFrontendFingerprintMovesWithEveryFrontendLimit. That migration is the
        // third one this helper has survived; check BuildTBuiltInResource before adding a field
        // here.
        a.params.MaxColorTextureSamples = 1;
        b.params.MaxColorTextureSamples = 8;
        a.params.MaxTextureSize = 4096;
        b.params.MaxTextureSize = 16384;
        a.params.MaxViewports = 1;
        b.params.MaxViewports = 16;
        a.params.MaxUniformBufferBindings = 24;
        b.params.MaxUniformBufferBindings = 84;
        a.params.MaxRenderbufferSize = 4096;
        b.params.MaxRenderbufferSize = 16384;
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

// The ONE capability bit that breaks that sharing, and the two halves of why it is placed where
// it is. It must NOT move the front-end fingerprint - glslang parses, reflects and generates a
// `double` identically under it, and L1c (the parse-verdict memo) keys on that same fingerprint
// and would take a false miss per backend for nothing. It MUST move the L1 key, because L1's
// payload is the module AFTER SanitizeAndOptimizeBinary and the fp64 tail of that chain is
// exactly what this bit gates.
TEST_F(TranslationCacheTest, NativeFloat64IsOutOfTheFrontendFingerprintAndInsideTheL1Key) {
    CompileEnv none;                              // no backend at all
    CompileEnv emulated;                          // a backend without the feature
    CompileEnv nativeEnv;                         // a backend with it
    emulated.backend = BackendType::DirectVulkan;
    nativeEnv.backend = BackendType::DirectVulkan;
    nativeEnv.params.SupportsShaderFloat64 = true;

    // No backend answers FALSE: the demoted module is the one that works everywhere, so a
    // standalone compile gets it.
    EXPECT_FALSE(none.ConsumesFloat64Natively());
    EXPECT_FALSE(emulated.ConsumesFloat64Natively());
    EXPECT_TRUE(nativeEnv.ConsumesFloat64Natively());

    EXPECT_EQ(ComputeFrontendCompileEnvFingerprint(emulated), ComputeFrontendCompileEnvFingerprint(nativeEnv))
        << "the fp64 capability leaked into the front-end fingerprint";
    EXPECT_NE(ComputeCompileEnvFingerprint(emulated), ComputeCompileEnvFingerprint(nativeEnv))
        << "the whole-environment fingerprint has to notice it - it is a DynamicBackendParameters "
           "field, hashed by object representation";

    const Vector<SpirvTranslationKeyInputs::Stage> stages{{GL_VERTEX_SHADER, kVertexSource},
                                                          {GL_FRAGMENT_SHADER, kFragmentSource}};
    SpirvTranslationKeyInputs demoted = BaselineSpirvInputs(stages);
    demoted.frontendFingerprint = ComputeFrontendCompileEnvFingerprint(emulated);
    demoted.nativeFloat64 = emulated.ConsumesFloat64Natively();
    SpirvTranslationKeyInputs kept = BaselineSpirvInputs(stages);
    kept.frontendFingerprint = ComputeFrontendCompileEnvFingerprint(nativeEnv);
    kept.nativeFloat64 = nativeEnv.ConsumesFloat64Natively();

    EXPECT_FALSE(BuildSpirvTranslationKey(demoted) == BuildSpirvTranslationKey(kept))
        << "one L1 entry would then describe two different module sets";
}

// The other direction, one case per input that was KEPT. Each is a limit the front end
// really consumes - everything BuildTBuiltInResource copies into TBuiltInResource, plus the
// two inputs to the reflection vertex-attrib limit - so each must still split the key.
// KEEP THIS LIST IN STEP WITH BuildTBuiltInResource: a limit that becomes env-derived there
// and is not added here is a silent miscompile with no failing test to catch it, which is
// precisely how the compute work-group cases below arrived.
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
        // Env-derived since wave3's cb155c5b: BuildTBuiltInResource copies all seven of
        // these into TBuiltInResource, and glslang expands each into a built-in constant a
        // compute shader can read (gl_MaxComputeTextureImageUnits,
        // gl_MaxComputeWorkGroupSize, gl_MaxComputeWorkGroupCount). A module that reads one
        // compiles to different SPIR-V under two different values, so each must split the
        // key - one case per COMPONENT, because a per-axis difference is exactly the shape
        // real drivers produce (z = 64 on ES against 1024 elsewhere).
        {"params.MaxComputeTextureImageUnits",
         [](CompileEnv& e) { e.params.MaxComputeTextureImageUnits += 1; }},
        // wave4's 4fc3531d: glslang rejects gl_ClipDistance[i] past this at parse AND expands
        // gl_MaxClipDistances from it, so it is both a compile gate and a baked constant.
        {"params.MaxClipDistances", [](CompileEnv& e) { e.params.MaxClipDistances += 1; }},
        // The GL 4.6 API-surface wave: six more TBuiltInResource fields that used to be stock
        // glslang literals. The cull pair is the MaxClipDistances story exactly (parse gate plus
        // gl_MaxCullDistances / gl_MaxCombinedClipAndCullDistances); the texture-image-unit three
        // and MaxSamples are baked constants (gl_MaxTextureImageUnits,
        // gl_MaxVertexTextureImageUnits, gl_MaxCombinedTextureImageUnits, gl_MaxSamples - the
        // last of which also sizes gl_SampleMask[]).
        {"params.MaxCullDistances", [](CompileEnv& e) { e.params.MaxCullDistances += 1; }},
        {"params.MaxCombinedClipAndCullDistances",
         [](CompileEnv& e) { e.params.MaxCombinedClipAndCullDistances += 1; }},
        {"params.MaxTextureImageUnits", [](CompileEnv& e) { e.params.MaxTextureImageUnits += 1; }},
        {"params.MaxVertexTextureImageUnits", [](CompileEnv& e) { e.params.MaxVertexTextureImageUnits += 1; }},
        {"params.MaxCombinedTextureImageUnits",
         [](CompileEnv& e) { e.params.MaxCombinedTextureImageUnits += 1; }},
        {"params.MaxSamples", [](CompileEnv& e) { e.params.MaxSamples += 1; }},
        {"maxComputeWorkGroupSize[0]", [](CompileEnv& e) { e.maxComputeWorkGroupSize[0] += 1; }},
        {"maxComputeWorkGroupSize[1]", [](CompileEnv& e) { e.maxComputeWorkGroupSize[1] += 1; }},
        {"maxComputeWorkGroupSize[2]", [](CompileEnv& e) { e.maxComputeWorkGroupSize[2] += 1; }},
        {"maxComputeWorkGroupCount[0]", [](CompileEnv& e) { e.maxComputeWorkGroupCount[0] += 1; }},
        {"maxComputeWorkGroupCount[1]", [](CompileEnv& e) { e.maxComputeWorkGroupCount[1] += 1; }},
        {"maxComputeWorkGroupCount[2]", [](CompileEnv& e) { e.maxComputeWorkGroupCount[2] += 1; }},
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

// glGetFragDataLocation on a program served from the memo.
//
// Split out from the case above because it caught a REAL bug that case did not: every
// accessor it checks had already been moved onto the owned reflection snapshot, but
// GetFragmentDataLocation still opened with `if (!Artifacts().program) return -1` and then
// walked the live TProgram's pipe outputs. On a hit there is no TProgram - that is the whole
// point of the memo - so the guard fired and the function reported "this program has no such
// fragment output" for an output that plainly exists. The failure mode was silent and
// asymmetric: the FIRST program with a given source answered correctly and every later one
// answered -1, so nothing that linked a program once could see it.
//
// Both the explicit-request path (glBindFragDataLocation, answered from
// linkedFragDataLocation) and the shader-declared path (layout(location = 0), answered from
// the pipe-output snapshot) are checked, because only the second one reads the field that
// used to come off the TProgram.
TEST_F(TranslationCacheTest, AProgramServedFromTheMemoStillAnswersGetFragDataLocation) {
    const SyncCompileScope sync;
    const CacheModeScope cacheOn(true);
    const String fs = SwizzleLikeFragment("");

    const GLuint parsed = LinkProgramFromSources(kVertexSource, fs);
    const TranslationCacheStats afterFirst = MG_State::GLState::GetProgramTranslationCache().Stats();
    const GLuint fromMemo = LinkProgramFromSources(kVertexSource, fs);
    const TranslationCacheStats afterSecond = MG_State::GLState::GetProgramTranslationCache().Stats();
    ASSERT_EQ(afterSecond.hits - afterFirst.hits, 1u) << "the second link was not a hit";

    const Int parsedLocation = MG_Impl::GLImpl::GetFragDataLocation(parsed, "fragColor");
    const Int memoLocation = MG_Impl::GLImpl::GetFragDataLocation(fromMemo, "fragColor");
    EXPECT_EQ(parsedLocation, 0) << "the parsed program's own answer moved; this case is testing "
                                    "the wrong thing";
    EXPECT_EQ(memoLocation, parsedLocation)
        << "a program served from the L1 memo lost its fragment output location";

    // A name that is not an output must still be -1 from both, so the case cannot pass by
    // making the accessor answer everything.
    EXPECT_EQ(MG_Impl::GLImpl::GetFragDataLocation(fromMemo, "notAnOutput"), -1);
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
    const std::map<String, String> ioBlockRenames{{"TCSOutputBlock", "TCSOutputBlock_mgio1"}};
    const std::map<String, String> otherIoBlockRenames{{"TCSOutputBlock", "TCSOutputBlock_mgio2"}};

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
        v.viewportIndexLoweringArmed = true;
        variants.emplace_back("viewportIndexLoweringArmed", BuildEsslTranslationKey(v));
    }
    {   // arms EmulateNoPerspectiveForEssl
        EsslTranslationKeyInputs v = base;
        v.supportsNoperspectiveInterpolation = true;
        variants.emplace_back("supportsNoperspectiveInterpolation", BuildEsslTranslationKey(v));
    }
    {   // arms WidenImageFormatsForEssl - a driver WITH GL_NV_image_formats keeps the declared
        // rg32f/r8ui/... image formats, one without has them re-declared in a core carrier and
        // every access to them masked, so the two get materially different ESSL from one module.
        EsslTranslationKeyInputs v = base;
        v.supportsExtendedImageFormats = true;
        variants.emplace_back("supportsExtendedImageFormats", BuildEsslTranslationKey(v));
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
    {   // SpvcSession::SetAtomicCounterBlockBindings - printed into the layout(binding=)
        // qualifier of every synthesized counter block, so it changes the emitted text.
        EsslTranslationKeyInputs v = base;
        v.atomicCounterEsslBindingTop = 6;
        variants.emplace_back("atomicCounterEsslBindingTop", BuildEsslTranslationKey(v));
    }
    {   // the two arguments to UniquifyIoBlockNamesForEssl. Separate cases, because a stage
        // that CONSUMES a block renames it after the previous stage while one that PRODUCES
        // it renames after itself - so the same block name legitimately maps to different
        // spellings in the two maps, and a key that folded them together would let a
        // consumer's plan be served to a producer.
        EsslTranslationKeyInputs v = base;
        v.inputBlockRenames = &ioBlockRenames;
        variants.emplace_back("inputBlockRenames", BuildEsslTranslationKey(v));
    }
    {
        EsslTranslationKeyInputs v = base;
        v.outputBlockRenames = &ioBlockRenames;
        variants.emplace_back("outputBlockRenames", BuildEsslTranslationKey(v));
    }
    {   // ...and a DIFFERENT target spelling for the same block name must not share either.
        EsslTranslationKeyInputs v = base;
        v.outputBlockRenames = &otherIoBlockRenames;
        variants.emplace_back("outputBlockRenames(other target)", BuildEsslTranslationKey(v));
    }
    {   // the two arguments to StripIoBlockLocationsForEssl, and separate cases for the same
        // reason the rename maps are: a stage strips the blocks it CONSUMES only when the
        // producer is in this program and the ones it PRODUCES only when the consumer is, so
        // the two directions are independently armed and a key that folded them together
        // would serve a fragment stage's ESSL to a vertex stage that needs the opposite.
        EsslTranslationKeyInputs v = base;
        v.stripInputBlockLocations = true;
        variants.emplace_back("stripInputBlockLocations", BuildEsslTranslationKey(v));
    }
    {
        EsslTranslationKeyInputs v = base;
        v.stripOutputBlockLocations = true;
        variants.emplace_back("stripOutputBlockLocations", BuildEsslTranslationKey(v));
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
    inputs.viewportIndexLoweringArmed = true;
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

// =========================================================================================
// L1c - the COMPILE half: the parse verdict memo
// =========================================================================================
//
// Same weighting as the two levels above: mostly the KEY, because an under-specified key
// here means a shader that reports GL_TRUE for a source that does not actually parse (or
// the reverse), which is the one way this level can produce a wrong answer.

namespace {
    ShaderParseVerdictKeyInputs BaselineParseVerdictInputs(const StringView source) {
        ShaderParseVerdictKeyInputs inputs;
        inputs.frontendFingerprint = 0x0f1e'2d3c'4b5a'6978ull;
        inputs.shaderType = GL_FRAGMENT_SHADER;
        inputs.preprocessedSource = source;
        inputs.shaderCompileFlags = 0;
        return inputs;
    }

    // A source no other case in this file uses, so the process-global L1c cache cannot be
    // pre-warmed by a neighbour and turn a "must miss" assertion green by accident.
    String UniqueFragment(const String& tag) {
        return "#version 460\n"
               "in vec3 vPos;\n"
               "layout(location = 0) out vec4 fragColor;\n"
               "uniform vec3 uTint_" + tag + ";\n"
               "void main() {\n"
               "    fragColor = vec4(uTint_" + tag + " * vPos, 1.0);\n"
               "}\n";
    }

    Bool ShaderHasParse(const GLuint shader) {
        const auto& object = MG_State::pGLContext->GetShaderObject(shader);
        return object != nullptr && object->GetCompiledShader() != nullptr;
    }

    String ShaderInfoLog(const GLuint shader) {
        const auto& object = MG_State::pGLContext->GetShaderObject(shader);
        return object ? object->GetInfoLog() : String();
    }

    GLint ShaderCompileStatus(const GLuint shader) {
        GLint status = GL_FALSE;
        MG_Impl::GLImpl::GetShaderiv(shader, GL_COMPILE_STATUS, &status);
        return status;
    }
} // namespace

// One case per input in the key inventory on ShaderParseVerdictKeyInputs. Each must move the
// key on its own, and none may collide with another - the same discipline the L1 and L2 key
// cases follow, and the test that catches an input someone forgot to add.
TEST_F(TranslationCacheTest, TheParseVerdictKeyMovesWithEveryInput) {
    const String source = UniqueFragment("keyinputs");
    const String otherSource = UniqueFragment("keyinputs_other");
    const TranslationCacheKey baseline = BuildShaderParseVerdictKey(BaselineParseVerdictInputs(source));

    const Vector<Pair<const char*, std::function<void(ShaderParseVerdictKeyInputs&, const String&)>>> mutations{
        {"frontendFingerprint",
         [](ShaderParseVerdictKeyInputs& i, const String&) { i.frontendFingerprint ^= 1ull; }},
        {"shaderType",
         [](ShaderParseVerdictKeyInputs& i, const String&) { i.shaderType = GL_VERTEX_SHADER; }},
        {"preprocessedSource",
         [](ShaderParseVerdictKeyInputs& i, const String& other) { i.preprocessedSource = other; }},
        {"shaderCompileFlags",
         [](ShaderParseVerdictKeyInputs& i, const String&) { i.shaderCompileFlags = 1u; }},
    };

    Vector<TranslationCacheKey> seen{baseline};
    for (const auto& [name, mutate] : mutations) {
        ShaderParseVerdictKeyInputs inputs = BaselineParseVerdictInputs(source);
        mutate(inputs, otherSource);
        const TranslationCacheKey moved = BuildShaderParseVerdictKey(inputs);
        EXPECT_FALSE(moved == baseline) << "moving " << name << " did not move the L1c key";
        for (const TranslationCacheKey& previous : seen) {
            EXPECT_FALSE(moved == previous) << name << " collides with an earlier L1c input";
        }
        seen.push_back(moved);
    }
}

// L1c inherits L1 backend-agnosticism by contract, so it gets L1 own case: two environments
// that differ in every way that only steers a BACKEND must share one entry.
TEST_F(TranslationCacheTest, TheParseVerdictKeyIgnoresBackendOnlyDifferences) {
    const auto [a, b] = BackendOnlyDifferentEnvs();
    const String source = UniqueFragment("agnostic");

    ShaderParseVerdictKeyInputs onA = BaselineParseVerdictInputs(source);
    onA.frontendFingerprint = ComputeFrontendCompileEnvFingerprint(a);
    ShaderParseVerdictKeyInputs onB = BaselineParseVerdictInputs(source);
    onB.frontendFingerprint = ComputeFrontendCompileEnvFingerprint(b);

    EXPECT_TRUE(BuildShaderParseVerdictKey(onA) == BuildShaderParseVerdictKey(onB));
}

// The headline behaviour: a second shader object handed the same source does not parse.
//
// Synchronous, deliberately. Under async, ShaderCompileAdoptionMap would hand the second
// object the FIRST one whole compile node and no second compile would run at all - a
// different (and older) optimisation, which would make this case pass without L1c existing.
TEST_F(TranslationCacheTest, ASecondCompileOfTheSameSourceSkipsTheParse) {
    const SyncCompileScope sync;
    const CacheModeScope cacheOn(true);
    const String fs = UniqueFragment("skipparse");

    const TranslationCacheStats before = GetShaderParseVerdictCache().Stats();
    const GLuint first = MakeShader(GL_FRAGMENT_SHADER, fs);
    const TranslationCacheStats afterFirst = GetShaderParseVerdictCache().Stats();
    const GLuint second = MakeShader(GL_FRAGMENT_SHADER, fs);
    const TranslationCacheStats afterSecond = GetShaderParseVerdictCache().Stats();

    EXPECT_EQ(afterFirst.misses - before.misses, 1u) << "the first compile must be a miss";
    EXPECT_EQ(afterSecond.hits - afterFirst.hits, 1u) << "the second compile must be an L1c hit";
    EXPECT_EQ(afterSecond.misses - afterFirst.misses, 0u);

    // Everything glCompileShader makes observable is identical...
    EXPECT_EQ(ShaderCompileStatus(first), GL_TRUE);
    EXPECT_EQ(ShaderCompileStatus(second), GL_TRUE);
    EXPECT_EQ(ShaderInfoLog(second), ShaderInfoLog(first));

    // ...and the second one really did skip the parse. This is the assertion that fails if
    // the level silently stops working; the counters above would still look right if the
    // publish path had been changed to parse anyway.
    EXPECT_TRUE(ShaderHasParse(first)) << "the first compile was supposed to parse";
    EXPECT_FALSE(ShaderHasParse(second)) << "the second compile parsed anyway; L1c did nothing";
}

// A failed compile is memoized too, and a hit has to reproduce the diagnostic the
// application would have read - not merely the GL_FALSE. An empty log on a failed compile is
// the least debuggable thing this driver can hand back, so it gets its own assertion.
TEST_F(TranslationCacheTest, AFailedCompileReproducesItsInfoLogFromTheMemo) {
    const SyncCompileScope sync;
    const CacheModeScope cacheOn(true);
    const String broken = "#version 460\n"
                          "layout(location = 0) out vec4 fragColor;\n"
                          "void main() { fragColor = this_function_does_not_exist(); }\n";

    const GLuint first = MakeShader(GL_FRAGMENT_SHADER, broken);
    const TranslationCacheStats afterFirst = GetShaderParseVerdictCache().Stats();
    const GLuint second = MakeShader(GL_FRAGMENT_SHADER, broken);
    const TranslationCacheStats afterSecond = GetShaderParseVerdictCache().Stats();

    EXPECT_EQ(ShaderCompileStatus(first), GL_FALSE);
    EXPECT_EQ(ShaderCompileStatus(second), GL_FALSE);
    EXPECT_FALSE(ShaderInfoLog(first).empty());
    EXPECT_EQ(ShaderInfoLog(second), ShaderInfoLog(first))
        << "a memoized compile failure lost the diagnostic the application reads";

    // The second compile is served by SOME memo - which one depends on whether the
    // per-context preprocess cache got there first (it records ParseFailed and short-circuits
    // ahead of L1c). Either way what must not happen is a second parse, so assert on the
    // thing both routes guarantee rather than on which route ran.
    EXPECT_EQ(afterSecond.misses - afterFirst.misses, 0u)
        << "the second compile of a known-bad source reached the parse again";
    EXPECT_FALSE(ShaderHasParse(second));
}

// The deferred parse, which is the half of this design that could quietly produce a wrong
// program: a stage whose compile hit L1c holds no AST, so a link that MISSES L1 has to parse
// it on demand - and the module it produces must be the one a from-scratch build produces.
//
// The shape forces exactly that: the same vertex source is linked into two programs with
// DIFFERENT fragment stages, so the second link vertex stage hits L1c (same source) while
// the program-level key misses (different fragment source).
TEST_F(TranslationCacheTest, AStageServedFromL1cStillLinksWhenTheProgramKeyMisses) {
    const SyncCompileScope sync;
    const String fsA = UniqueFragment("deferred_a");
    const String fsB = UniqueFragment("deferred_b");

    // The reference: the whole chain with every memo off.
    Vector<Uint64> uncachedB;
    {
        const CacheModeScope cacheOff(false);
        uncachedB = ProgramSpirvDigest(LinkProgramFromSources(kVertexSource, fsB));
    }
    ASSERT_EQ(uncachedB.size(), 2u);

    const CacheModeScope cacheOn(true);
    // Program 1 populates L1c for kVertexSource.
    const GLuint programA = LinkProgramFromSources(kVertexSource, fsA);
    GLint statusA = GL_FALSE;
    MG_Impl::GLImpl::GetProgramiv(programA, GL_LINK_STATUS, &statusA);
    ASSERT_EQ(statusA, GL_TRUE);

    const TranslationCacheStats l1Before = MG_State::GLState::GetProgramTranslationCache().Stats();
    const TranslationCacheStats l1cBefore = GetShaderParseVerdictCache().Stats();
    const GLuint programB = LinkProgramFromSources(kVertexSource, fsB);
    const TranslationCacheStats l1After = MG_State::GLState::GetProgramTranslationCache().Stats();
    const TranslationCacheStats l1cAfter = GetShaderParseVerdictCache().Stats();

    ASSERT_EQ(l1cAfter.hits - l1cBefore.hits, 1u) << "the shared vertex stage was supposed to hit L1c";
    ASSERT_EQ(l1After.misses - l1Before.misses, 1u) << "the program key was supposed to miss";

    GLint statusB = GL_FALSE;
    MG_Impl::GLImpl::GetProgramiv(programB, GL_LINK_STATUS, &statusB);
    EXPECT_EQ(statusB, GL_TRUE) << "the deferred parse failed to produce a linkable stage";
    EXPECT_EQ(ProgramSpirvDigest(programB), uncachedB)
        << "a stage parsed lazily at link time produced different SPIR-V from one parsed at compile time";
}

// MOBILEGL_SHADER_CACHE=0 has to reach this level too. It is the switch that isolated the
// static-destruction-order heap corruption in the first place, so a level it does not cover
// is a level that cannot be bisected against.
TEST_F(TranslationCacheTest, TheEscapeHatchDisablesL1c) {
    const SyncCompileScope sync;
    const CacheModeScope cacheOff(false);
    const String fs = UniqueFragment("escapehatch");

    const TranslationCacheStats before = GetShaderParseVerdictCache().Stats();
    const GLuint first = MakeShader(GL_FRAGMENT_SHADER, fs);
    const GLuint second = MakeShader(GL_FRAGMENT_SHADER, fs);
    const TranslationCacheStats after = GetShaderParseVerdictCache().Stats();

    EXPECT_EQ(after.hits - before.hits, 0u);
    EXPECT_EQ(after.misses - before.misses, 0u) << "the cache was consulted with the escape hatch set";
    EXPECT_EQ(after.inserts - before.inserts, 0u);
    // With the level off, BOTH compiles parse - which is the pre-L1c behaviour exactly.
    EXPECT_TRUE(ShaderHasParse(first));
    EXPECT_TRUE(ShaderHasParse(second));
}

// The whole point, end to end and in the CTS shape: N programs from fresh shader objects
// over one pair of sources. After the first, no program constructs a glslang object at all -
// no parse (L1c) and no link, mapIO, GlslangToSpv or reflection (L1).
TEST_F(TranslationCacheTest, TheCtsShapeStopsParsingAfterTheFirstProgram) {
    const SyncCompileScope sync;
    const CacheModeScope cacheOn(true);
    const String fs = UniqueFragment("ctsshape");
    constexpr Uint kPrograms = 8;

    const GLuint firstProgram = LinkProgramFromSources(kVertexSource, fs);
    GLint firstStatus = GL_FALSE;
    MG_Impl::GLImpl::GetProgramiv(firstProgram, GL_LINK_STATUS, &firstStatus);
    ASSERT_EQ(firstStatus, GL_TRUE);
    const Vector<Uint64> firstDigest = ProgramSpirvDigest(firstProgram);

    const TranslationCacheStats l1Before = MG_State::GLState::GetProgramTranslationCache().Stats();
    const TranslationCacheStats l1cBefore = GetShaderParseVerdictCache().Stats();
    for (Uint i = 1; i < kPrograms; ++i) {
        const GLuint program = LinkProgramFromSources(kVertexSource, fs);
        GLint status = GL_FALSE;
        MG_Impl::GLImpl::GetProgramiv(program, GL_LINK_STATUS, &status);
        ASSERT_EQ(status, GL_TRUE) << "program " << i;
        EXPECT_EQ(ProgramSpirvDigest(program), firstDigest) << "program " << i;
    }
    const TranslationCacheStats l1After = MG_State::GLState::GetProgramTranslationCache().Stats();
    const TranslationCacheStats l1cAfter = GetShaderParseVerdictCache().Stats();

    // Two stages per program, and every one of them served from the memo.
    EXPECT_EQ(l1cAfter.hits - l1cBefore.hits, 2u * (kPrograms - 1));
    EXPECT_EQ(l1cAfter.misses - l1cBefore.misses, 0u) << "a stage reached the glslang parse again";
    EXPECT_EQ(l1After.hits - l1Before.hits, kPrograms - 1);
    EXPECT_EQ(l1After.misses - l1Before.misses, 0u);
}
