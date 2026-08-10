// MobileGL - MobileGL/MG_Test/Program/AsyncLinkTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P1 stage 4: glLinkProgram enqueues a ProgramLinkTask behind its shaders' compiles, and
// every observable read of link output joins.
//
// Like AsyncCompileTest, every case here drives the real GL entry points and flips
// MG_Config::Features.AsyncShaderCompile itself rather than reading the environment - so one
// binary can assert the property that actually matters (the async and synchronous paths are
// indistinguishable through the GL surface) regardless of how the suite was launched.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "Config.h"
#include "Includes.h"
#include "Init.h"
#include "MG_Impl/GLImpl/Getter/GL_Getter.h"
#include "MG_Impl/GLImpl/Program/GL_Program.h"
#include "MG_Impl/GLImpl/Program/GL_ProgramPipeline.h"
#include "MG_State/GLState/Core.h"
#include "MG_Util/Async/ShaderCompilePool.h"

using namespace MobileGL;
using namespace MobileGL::MG_Impl::GLImpl;

namespace {
    class AsyncModeScope {
    public:
        explicit AsyncModeScope(const Bool async) : m_saved(MG_Config::Features.AsyncShaderCompile) {
            MG_Config::Features.AsyncShaderCompile =
                async ? MG_Config::QuirkOverride::ForceOn : MG_Config::QuirkOverride::ForceOff;
        }
        ~AsyncModeScope() { MG_Config::Features.AsyncShaderCompile = m_saved; }
        AsyncModeScope(const AsyncModeScope&) = delete;
        AsyncModeScope& operator=(const AsyncModeScope&) = delete;

    private:
        const MG_Config::QuirkOverride m_saved;
    };

    const char* kVs = R"(#version 460
layout(location = 0) in vec3 aPos;
uniform mat4 uModel;
uniform vec4 uColor;
out vec4 vColor;
void main() {
    vColor = uColor;
    gl_Position = uModel * vec4(aPos, 1.0);
}
)";

    const char* kFs = R"(#version 460
in vec4 vColor;
layout(location = 0) out vec4 fragColor;
uniform float uAlpha;
void main() { fragColor = vec4(vColor.rgb, vColor.a * uAlpha); }
)";

    // A vertex shader that captures something transform feedback can name.
    const char* kXfbVs = R"(#version 460
layout(location = 0) in vec3 aPos;
out vec3 vWorld;
void main() {
    vWorld = aPos * 2.0;
    gl_Position = vec4(aPos, 1.0);
}
)";

    const char* kBrokenFs = R"(#version 460
layout(location = 0) out vec4 fragColor;
void main() { fragColor = thisIdentifierWasNeverDeclared; }
)";

    // Big enough that neither the compile nor the link is instantaneous, so the pool has a
    // real backlog to race against. Templated on an index so every instance is distinct
    // source text (no P0b memo hit).
    String MakeBulkySource(const int index) {
        String source = "#version 460\nlayout(location = 0) out vec4 fragColor;\n";
        source += "uniform float uSeed" + std::to_string(index) + ";\n";
        source += "void main() {\n    float acc = uSeed" + std::to_string(index) + ";\n";
        for (int i = 0; i < 220; ++i) {
            source += "    acc = acc * 1.0001 + sin(acc + " + std::to_string(i) + ".0) * cos(acc);\n";
        }
        source += "    fragColor = vec4(acc, acc, acc, 1.0);\n}\n";
        return source;
    }

    GLuint MakeShader(const GLenum type, const char* source) {
        const GLuint shader = CreateShader(type);
        ShaderSource(shader, 1, &source, nullptr);
        CompileShader(shader);
        return shader;
    }

    GLint QueryLinkStatus(const GLuint program) {
        GLint status = GL_FALSE;
        GetProgramiv(program, GL_LINK_STATUS, &status);
        return status;
    }

    String QueryProgramInfoLog(const GLuint program) {
        GLint length = 0;
        GetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
        if (length <= 0) return String();
        std::vector<GLchar> buffer(static_cast<size_t>(length));
        GLsizei written = 0;
        GetProgramInfoLog(program, length, &written, buffer.data());
        return String(buffer.data(), static_cast<size_t>(written));
    }

    // The non-joining view of the program, i.e. what GL_COMPLETION_STATUS_KHR will report.
    // BOTH phases: a program whose SPIR-V job is still in flight is not finished, even though
    // its whole GL query surface already answers.
    Bool LinkIsSettled(const GLuint program) {
        const auto& object = MG_State::pGLContext->GetProgramObject(program);
        return object == nullptr || object->IsLinkComplete();
    }

    // Phase A alone: the half that decides LINK_STATUS, the info log, and every reflection
    // query. This is what a read of LINK_STATUS is required to settle.
    Bool PhaseALinkIsSettled(const GLuint program) {
        const auto& object = MG_State::pGLContext->GetProgramObject(program);
        return object == nullptr || object->IsPhaseALinkComplete();
    }

    // Phase B alone: SPIR-V + the uniform shadow's layout.
    Bool SpirvIsSettled(const GLuint program) {
        const auto& object = MG_State::pGLContext->GetProgramObject(program);
        return object == nullptr || object->IsSpirvComplete();
    }

    // Enqueues `count` distinct heavy compiles without reading anything back, so the pool is
    // left with a real backlog for the caller to race against.
    Vector<GLuint> SaturatePool(const int count, Vector<String>& sourceStorage) {
        Vector<GLuint> shaders;
        shaders.reserve(static_cast<SizeT>(count));
        sourceStorage.reserve(sourceStorage.size() + static_cast<SizeT>(count));
        for (int i = 0; i < count; ++i) {
            sourceStorage.push_back(MakeBulkySource(20000 + i));
            const char* text = sourceStorage.back().c_str();
            const GLuint shader = CreateShader(GL_FRAGMENT_SHADER);
            ShaderSource(shader, 1, &text, nullptr);
            CompileShader(shader);
            shaders.push_back(shader);
        }
        return shaders;
    }

    // Content hash of a linked program's generated SPIR-V, through the state layer (there is
    // no GL query for it). Joins, like every other artifact read.
    Vector<Uint64> SpirvDigest(const GLuint program) {
        const auto& object = MG_State::pGLContext->GetProgramObject(program);
        Vector<Uint64> digest;
        if (!object) return digest;
        for (const auto& module : object->GetGeneratedSpirv()) {
            Uint64 hash = 1469598103934665603ull;
            for (const unsigned word : module) {
                hash = (hash ^ static_cast<Uint64>(word)) * 1099511628211ull;
            }
            digest.push_back(hash);
        }
        return digest;
    }

    class AsyncLinkTest : public ::testing::Test {
    protected:
        void SetUp() override { MobileGL::Initialize(); }
    };
} // namespace

// ---------------------------------------------------------------------------------------
// The consume-once claim
// ---------------------------------------------------------------------------------------

// The stage-4 headline risk: two programs share one shader and are linked back to back, so
// two ProgramLinkTasks race for that shader's single glslang parse. Exactly one may win the
// claim; the loser must re-parse the same preprocessed source against the same CompileEnv.
// If either half of that is wrong the two programs get DIFFERENT SPIR-V for the same shader,
// which is the silent-corruption class this whole mechanism exists to prevent.
TEST_F(AsyncLinkTest, TwoProgramsSharingAShaderGenerateIdenticalSpirv) {
    for (const Bool async : {false, true}) {
        const AsyncModeScope scope(async);

        const GLuint vs = MakeShader(GL_VERTEX_SHADER, kVs);
        const GLuint fs = MakeShader(GL_FRAGMENT_SHADER, kFs);

        // Both links enqueued before either result is read: with the flag on this is the
        // window in which two workers can hold the same node at once.
        const GLuint programA = CreateProgram();
        AttachShader(programA, vs);
        AttachShader(programA, fs);
        LinkProgram(programA);

        const GLuint programB = CreateProgram();
        AttachShader(programB, vs);
        AttachShader(programB, fs);
        LinkProgram(programB);

        ASSERT_EQ(QueryLinkStatus(programA), GL_TRUE) << QueryProgramInfoLog(programA);
        ASSERT_EQ(QueryLinkStatus(programB), GL_TRUE) << QueryProgramInfoLog(programB);

        const Vector<Uint64> digestA = SpirvDigest(programA);
        const Vector<Uint64> digestB = SpirvDigest(programB);
        ASSERT_EQ(digestA.size(), 2u) << "async=" << async;
        EXPECT_EQ(digestA, digestB)
            << "the claim winner and the re-parsing loser must produce identical SPIR-V (async=" << async << ")";

        // And the two programs really are usable independently.
        EXPECT_GE(GetUniformLocation(programA, "uColor"), 0);
        EXPECT_GE(GetUniformLocation(programB, "uColor"), 0);
        EXPECT_EQ(GetError(), GL_NO_ERROR);
    }
}

// The same property many ways at once, with the pool loaded: N programs over the SAME shader
// pair, all enqueued before anything is read, so one claim winner is racing N-1 re-parsers.
// Every program must come out byte-identical.
//
// The shader pair has to be identical across the programs for this to mean anything: glslang
// links the stages together, so a stage's SPIR-V is legitimately a function of the WHOLE
// program (mapIO's cross-stage location assignment, live-variable analysis). Comparing one
// shared vertex shader across programs with different fragment stages would compare things
// that are allowed to differ.
TEST_F(AsyncLinkTest, ManyProgramsSharingOneShaderPairAgreeOnTheirSpirv) {
    const AsyncModeScope async(true);
    Vector<String> backlog;
    SaturatePool(48, backlog);

    constexpr int kPrograms = 12;
    const GLuint vs = MakeShader(GL_VERTEX_SHADER, kVs);
    const GLuint fs = MakeShader(GL_FRAGMENT_SHADER, kFs);

    Vector<GLuint> programs;
    for (int i = 0; i < kPrograms; ++i) {
        const GLuint program = CreateProgram();
        AttachShader(program, vs);
        AttachShader(program, fs);
        LinkProgram(program);
        programs.push_back(program);
    }

    Vector<Uint64> reference;
    for (int i = 0; i < kPrograms; ++i) {
        const GLuint program = programs[static_cast<SizeT>(i)];
        ASSERT_EQ(QueryLinkStatus(program), GL_TRUE) << "program " << i << ": " << QueryProgramInfoLog(program);
        const Vector<Uint64> digest = SpirvDigest(program);
        ASSERT_EQ(digest.size(), 2u);
        if (i == 0) {
            reference = digest;
        } else {
            EXPECT_EQ(digest, reference) << "SPIR-V differs in program " << i;
        }
        EXPECT_GE(GetUniformLocation(program, "uColor"), 0) << "program " << i;
        EXPECT_GE(GetUniformLocation(program, "uAlpha"), 0) << "program " << i;
    }
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// ---------------------------------------------------------------------------------------
// Mutation over a pending link (the cancel matrix)
// ---------------------------------------------------------------------------------------

// The last link wins. A re-link over a pending one cancels it and enqueues afresh; the
// result the application eventually reads must be the SECOND link's.
TEST_F(AsyncLinkTest, RelinkOverAPendingLinkPublishesTheSecondLink) {
    const AsyncModeScope async(true);
    Vector<String> backlog;
    SaturatePool(48, backlog);

    const GLuint vs = MakeShader(GL_VERTEX_SHADER, kVs);
    const String firstSource = MakeBulkySource(7001);
    const char* firstText = firstSource.c_str();
    const GLuint fs = CreateShader(GL_FRAGMENT_SHADER);
    ShaderSource(fs, 1, &firstText, nullptr);
    CompileShader(fs);

    const GLuint program = CreateProgram();
    AttachShader(program, vs);
    AttachShader(program, fs);
    LinkProgram(program);

    // Swap the fragment shader's source and relink, all without ever reading the first
    // link's status - so the first link is very probably still queued or running.
    const String secondSource = MakeBulkySource(7002);
    const char* secondText = secondSource.c_str();
    ShaderSource(fs, 1, &secondText, nullptr);
    CompileShader(fs);
    LinkProgram(program);

    ASSERT_EQ(QueryLinkStatus(program), GL_TRUE) << QueryProgramInfoLog(program);
    EXPECT_GE(GetUniformLocation(program, "uSeed7002"), 0);
    EXPECT_EQ(GetUniformLocation(program, "uSeed7001"), -1);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// The take-effect-at-next-link setters must NOT disturb a pending link: the pending link
// snapshotted its own inputs at enqueue, so
//   glLinkProgram; glTransformFeedbackVaryings; glGetProgramiv(LINK_STATUS)
// has to report the FIRST link - which captured nothing.
TEST_F(AsyncLinkTest, TransformFeedbackVaryingsOverAPendingLinkReportsTheFirstLink) {
    const AsyncModeScope async(true);
    Vector<String> backlog;
    SaturatePool(48, backlog);

    const GLuint vs = MakeShader(GL_VERTEX_SHADER, kXfbVs);
    const GLuint fs = MakeShader(GL_FRAGMENT_SHADER, kFs);
    const GLuint program = CreateProgram();
    AttachShader(program, vs);
    AttachShader(program, fs);
    LinkProgram(program);

    const char* varyings[] = {"vWorld"};
    TransformFeedbackVaryings(program, 1, varyings, GL_INTERLEAVED_ATTRIBS);

    ASSERT_EQ(QueryLinkStatus(program), GL_TRUE) << QueryProgramInfoLog(program);
    GLint captured = -1;
    GetProgramiv(program, GL_TRANSFORM_FEEDBACK_VARYINGS, &captured);
    EXPECT_EQ(captured, 0) << "the pending link must publish the request set it snapshotted, not a later one";

    // And the request does take effect at the NEXT link.
    LinkProgram(program);
    ASSERT_EQ(QueryLinkStatus(program), GL_TRUE) << QueryProgramInfoLog(program);
    GetProgramiv(program, GL_TRANSFORM_FEEDBACK_VARYINGS, &captured);
    EXPECT_EQ(captured, 1);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// glBindAttribLocation is the same family and must likewise leave a pending link alone.
TEST_F(AsyncLinkTest, BindAttribLocationOverAPendingLinkDoesNotDisturbIt) {
    const AsyncModeScope async(true);
    Vector<String> backlog;
    SaturatePool(48, backlog);

    const GLuint vs = MakeShader(GL_VERTEX_SHADER, kVs);
    const GLuint fs = MakeShader(GL_FRAGMENT_SHADER, kFs);
    const GLuint program = CreateProgram();
    AttachShader(program, vs);
    AttachShader(program, fs);
    LinkProgram(program);

    BindAttribLocation(program, 5, "aPos");
    ASSERT_EQ(QueryLinkStatus(program), GL_TRUE) << QueryProgramInfoLog(program);
    EXPECT_EQ(GetAttribLocation(program, "aPos"), 0) << "the first link's layout(location = 0) must survive";

    LinkProgram(program);
    ASSERT_EQ(QueryLinkStatus(program), GL_TRUE) << QueryProgramInfoLog(program);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// glAttachShader after glLinkProgram is defined to leave the current link status alone (it
// takes effect at the next link). It must therefore NOT cancel a pending link - the failure
// mode being guarded here is a program that linked fine reporting GL_FALSE.
TEST_F(AsyncLinkTest, AttachShaderOverAPendingLinkKeepsTheLinkResult) {
    const AsyncModeScope async(true);
    Vector<String> backlog;
    SaturatePool(48, backlog);

    const GLuint vs = MakeShader(GL_VERTEX_SHADER, kVs);
    const GLuint fs = MakeShader(GL_FRAGMENT_SHADER, kFs);
    const GLuint program = CreateProgram();
    AttachShader(program, vs);
    AttachShader(program, fs);
    LinkProgram(program);

    // A second, unrelated fragment shader attached over the pending link. (Attaching two
    // shaders of one stage is legal; only the next link would have to reconcile them.)
    const GLuint extraFs = MakeShader(GL_FRAGMENT_SHADER, kBrokenFs);
    AttachShader(program, extraFs);

    EXPECT_EQ(QueryLinkStatus(program), GL_TRUE) << QueryProgramInfoLog(program);
    EXPECT_GE(GetUniformLocation(program, "uAlpha"), 0);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// The link-then-detach-then-delete teardown every LWJGL/Blaze3D-shaped app performs. The
// detach makes the shader GL-invisible, so glDeleteShader frees its name and would otherwise
// cancel a compile the enqueued link is still waiting on - flipping a link that must report
// GL_TRUE to GL_FALSE. Runs with the pool saturated so the compiles really are outstanding.
TEST_F(AsyncLinkTest, DetachAndDeleteShadersOverAPendingLinkKeepsTheLinkResult) {
    const AsyncModeScope async(true);
    Vector<String> backlog;
    SaturatePool(48, backlog);

    const String source = MakeBulkySource(7400);
    const char* text = source.c_str();
    const GLuint vs = MakeShader(GL_VERTEX_SHADER, kVs);
    const GLuint fs = CreateShader(GL_FRAGMENT_SHADER);
    ShaderSource(fs, 1, &text, nullptr);
    CompileShader(fs);

    const GLuint program = CreateProgram();
    AttachShader(program, vs);
    AttachShader(program, fs);
    LinkProgram(program);

    DetachShader(program, vs);
    DetachShader(program, fs);
    DeleteShader(vs);
    DeleteShader(fs);
    EXPECT_EQ(IsShader(vs), GL_FALSE);
    EXPECT_EQ(IsShader(fs), GL_FALSE);

    ASSERT_EQ(QueryLinkStatus(program), GL_TRUE) << QueryProgramInfoLog(program);
    EXPECT_GE(GetUniformLocation(program, "uSeed7400"), 0);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// glCreateShaderProgramv is specified as create-source-compile-create-attach-LINK-detach, so
// it is the in-tree caller that exercises the detach-immediately-after-link ordering. It
// self-joins through its status queries (design join site J7) and needs no edit of its own -
// this is the guard that says so.
TEST_F(AsyncLinkTest, CreateShaderProgramvLinksUnderAsync) {
    const AsyncModeScope async(true);
    Vector<String> backlog;
    SaturatePool(48, backlog);

    const char* sources[] = {kVs};
    const GLuint program = CreateShaderProgramv(GL_VERTEX_SHADER, 1, sources);
    ASSERT_NE(program, 0u);
    EXPECT_EQ(QueryLinkStatus(program), GL_TRUE) << QueryProgramInfoLog(program);
    EXPECT_GE(GetUniformLocation(program, "uColor"), 0);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// glProgramBinary over a pending link: no format is supported, so the spec requires
// LINK_STATUS to read FALSE afterwards. The pending link must not publish over that.
TEST_F(AsyncLinkTest, ProgramBinaryOverAPendingLinkForcesLinkFalse) {
    const AsyncModeScope async(true);
    Vector<String> backlog;
    SaturatePool(48, backlog);

    const GLuint vs = MakeShader(GL_VERTEX_SHADER, kVs);
    const GLuint fs = MakeShader(GL_FRAGMENT_SHADER, kFs);
    const GLuint program = CreateProgram();
    AttachShader(program, vs);
    AttachShader(program, fs);
    LinkProgram(program);

    const GLuint dummy = 0;
    ProgramBinary(program, 0, &dummy, static_cast<GLsizei>(sizeof(dummy)));
    EXPECT_EQ(GetError(), GL_INVALID_ENUM);

    EXPECT_EQ(QueryLinkStatus(program), GL_FALSE) << "glProgramBinary must win over the pending link";
    EXPECT_FALSE(QueryProgramInfoLog(program).empty());
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// glDeleteProgram over a pending link. The name goes away immediately - no wait for a worker
// - and the abandoned job must neither crash nor keep anything observable alive.
TEST_F(AsyncLinkTest, DeleteProgramWhileALinkIsPending) {
    const AsyncModeScope async(true);
    Vector<String> backlog;
    SaturatePool(48, backlog);

    Vector<GLuint> doomed;
    Vector<String> sources;
    const GLuint vs = MakeShader(GL_VERTEX_SHADER, kVs);
    for (int i = 0; i < 16; ++i) {
        sources.push_back(MakeBulkySource(7500 + i));
        const char* text = sources.back().c_str();
        const GLuint fs = CreateShader(GL_FRAGMENT_SHADER);
        ShaderSource(fs, 1, &text, nullptr);
        CompileShader(fs);

        const GLuint program = CreateProgram();
        AttachShader(program, vs);
        AttachShader(program, fs);
        LinkProgram(program);
        doomed.push_back(program);
    }
    for (const GLuint program : doomed) {
        DeleteProgram(program);
        EXPECT_EQ(IsProgram(program), GL_FALSE) << "an unused deleted program's name goes immediately";
    }
    EXPECT_EQ(GetError(), GL_NO_ERROR);

    // The context still works afterwards: the abandoned links did not take the pool, the
    // preprocess cache or the glslang process state down with them.
    const GLuint fs = MakeShader(GL_FRAGMENT_SHADER, kFs);
    const GLuint program = CreateProgram();
    AttachShader(program, vs);
    AttachShader(program, fs);
    LinkProgram(program);
    EXPECT_EQ(QueryLinkStatus(program), GL_TRUE) << QueryProgramInfoLog(program);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// ---------------------------------------------------------------------------------------
// The join gates
// ---------------------------------------------------------------------------------------

// glLinkProgram must return before the work is done, and the first observable read must
// join. Observed through the state machine rather than through timing, so it can never be a
// false red: with a saturated pool at least one of the just-enqueued links has to be
// unsettled at the moment we ask; skipped if the machine drained everything first.
TEST_F(AsyncLinkTest, LinkProgramReturnsBeforeTheWorkIsDone) {
    const AsyncModeScope async(true);
    constexpr int kPrograms = 32;

    const GLuint vs = MakeShader(GL_VERTEX_SHADER, kVs);
    Vector<GLuint> programs;
    Vector<String> sources;
    for (int i = 0; i < kPrograms; ++i) {
        sources.push_back(MakeBulkySource(7600 + i));
        const char* text = sources.back().c_str();
        const GLuint fs = CreateShader(GL_FRAGMENT_SHADER);
        ShaderSource(fs, 1, &text, nullptr);
        CompileShader(fs);
        const GLuint program = CreateProgram();
        AttachShader(program, vs);
        AttachShader(program, fs);
        LinkProgram(program);
        programs.push_back(program);
    }

    int unsettled = 0;
    for (const GLuint program : programs) {
        if (!LinkIsSettled(program)) ++unsettled;
    }
    if (unsettled == 0) {
        GTEST_SKIP() << "the pool drained every link before the first observation; nothing to prove here";
    }

    for (const GLuint program : programs) {
        EXPECT_EQ(QueryLinkStatus(program), GL_TRUE) << QueryProgramInfoLog(program);
        // PHASE A only. Reading LINK_STATUS settles the half that decides it, and no more -
        // the SPIR-V job may well still be running, which is the entire point of the split.
        EXPECT_TRUE(PhaseALinkIsSettled(program)) << "reading LINK_STATUS must have joined phase A";
    }
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// The other half of the previous case, and the property the two-phase split exists for:
// LINK_STATUS is answerable without the SPIR-V, so a run of LINK_STATUS reads over a
// backlog must leave SPIR-V jobs outstanding rather than draining them one by one.
TEST_F(AsyncLinkTest, ReadingLinkStatusDoesNotSettleTheSpirvJob) {
    const AsyncModeScope async(true);
    MG_Util::Async::ShaderCompilePool::Get().SetMaxConcurrency(1);
    constexpr int kPrograms = 24;

    const GLuint vs = MakeShader(GL_VERTEX_SHADER, kVs);
    Vector<GLuint> programs;
    Vector<String> sources;
    for (int i = 0; i < kPrograms; ++i) {
        sources.push_back(MakeBulkySource(7900 + i));
        const char* text = sources.back().c_str();
        const GLuint fs = CreateShader(GL_FRAGMENT_SHADER);
        ShaderSource(fs, 1, &text, nullptr);
        CompileShader(fs);
        const GLuint program = CreateProgram();
        AttachShader(program, vs);
        AttachShader(program, fs);
        LinkProgram(program);
        programs.push_back(program);
    }

    int spirvOutstanding = 0;
    for (int i = 0; i < kPrograms; ++i) {
        const GLuint program = programs[static_cast<SizeT>(i)];
        EXPECT_EQ(QueryLinkStatus(program), GL_TRUE) << QueryProgramInfoLog(program);
        EXPECT_TRUE(PhaseALinkIsSettled(program)) << "reading LINK_STATUS must have joined phase A";
        // Reflection has to answer here too, out of phase A and with no further join.
        const String uniformName = "uSeed" + std::to_string(7900 + i);
        EXPECT_GE(GetUniformLocation(program, uniformName.c_str()), 0) << uniformName;
        if (!SpirvIsSettled(program)) ++spirvOutstanding;
    }
    EXPECT_GT(spirvOutstanding, 0) << "the whole GL query surface was answered and yet every SPIR-V job had "
                                      "already been drained - the reads are joining phase B";

    // And the SPIR-V gate really is a gate: touching it settles the job.
    for (const GLuint program : programs) {
        const auto& object = MG_State::pGLContext->GetProgramObject(program);
        ASSERT_NE(object, nullptr);
        EXPECT_GT(object->GetGeneratedSpirv().size(), 0u);
        EXPECT_TRUE(SpirvIsSettled(program));
        EXPECT_TRUE(LinkIsSettled(program));
    }
    EXPECT_EQ(GetError(), GL_NO_ERROR);
    MG_Util::Async::ShaderCompilePool::Get().SetMaxConcurrency(
        MG_Util::Async::ShaderCompilePool::Get().GetThreadCount());
}

// With the flag off, a link is finished by the time glLinkProgram returns. This is the guard
// that keeps the default shippable.
TEST_F(AsyncLinkTest, LinkIsFullySynchronousWithAsyncOff) {
    const AsyncModeScope async(false);
    ASSERT_FALSE(MG_Util::Async::AsyncShaderCompileEnabled());

    const GLuint vs = MakeShader(GL_VERTEX_SHADER, kVs);
    const GLuint fs = MakeShader(GL_FRAGMENT_SHADER, kFs);
    const GLuint program = CreateProgram();
    AttachShader(program, vs);
    AttachShader(program, fs);
    LinkProgram(program);
    EXPECT_TRUE(LinkIsSettled(program));
    EXPECT_EQ(QueryLinkStatus(program), GL_TRUE) << QueryProgramInfoLog(program);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// P1 join site J1: the composite draw program for a pipeline is cached against a signature
// built from each stage program's lifetime id and backend state version - NON-artifact
// fields, which do not pass through the join gate. GetProgramForDraw has to settle the stage
// programs first, or the signature describes a link generation that no longer exists and the
// composite is rebuilt on every draw.
TEST_F(AsyncLinkTest, DrawThroughAPipelineWithAPendingStageProgramJoinsFirst) {
    const AsyncModeScope async(true);
    Vector<String> backlog;
    SaturatePool(48, backlog);

    // Built by hand rather than through glCreateShaderProgramv: that entry point detaches the
    // shader immediately after linking, so the next link would remove it and leave the stage
    // program with nothing attached to composite from.
    const GLuint vs = MakeShader(GL_VERTEX_SHADER, kVs);
    const GLuint vsProgram = CreateProgram();
    ProgramParameteri(vsProgram, GL_PROGRAM_SEPARABLE, GL_TRUE);
    AttachShader(vsProgram, vs);
    LinkProgram(vsProgram);
    ASSERT_EQ(QueryLinkStatus(vsProgram), GL_TRUE) << QueryProgramInfoLog(vsProgram);

    GLuint pipeline = 0;
    GenProgramPipelines(1, &pipeline);
    ASSERT_NE(pipeline, 0u);
    // Bind before UseProgramStages: glGenProgramPipelines only reserves the name, and the
    // first bind is what turns it into an object glUseProgramStages can find.
    BindProgramPipeline(pipeline);
    UseProgramStages(pipeline, GL_VERTEX_SHADER_BIT, vsProgram);
    ASSERT_EQ(GetError(), GL_NO_ERROR);

    // Re-link the stage program and immediately ask for the draw program, without reading
    // the link's status in between: the pending link is what J1 has to settle.
    LinkProgram(vsProgram);
    const SharedPtr<MG_State::GLState::ProgramObject> drawProgram = MG_State::pGLContext->GetProgramForDraw();
    ASSERT_NE(drawProgram, nullptr);
    EXPECT_TRUE(LinkIsSettled(vsProgram)) << "GetProgramForDraw must have joined the stage program";
    EXPECT_TRUE(drawProgram->GetLinkStatus()) << drawProgram->GetInfoLog();

    // Asking again with nothing changed must hit the composite cache, which is only possible
    // if the signature was computed against settled programs both times.
    const SharedPtr<MG_State::GLState::ProgramObject> again = MG_State::pGLContext->GetProgramForDraw();
    EXPECT_EQ(again.get(), drawProgram.get()) << "the composite draw program must be cached across draws";
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// ---------------------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------------------

// A link whose fragment shader failed to compile has to reproduce that shader's log verbatim
// inside the program info log, whichever thread produced it - and the failure must be
// reported as LINK_STATUS plus a log, never as a GL error.
TEST_F(AsyncLinkTest, FailingLinkLogIsIdenticalAcrossModes) {
    String syncLog;
    {
        const AsyncModeScope scope(false);
        const GLuint vs = MakeShader(GL_VERTEX_SHADER, kVs);
        const GLuint fs = MakeShader(GL_FRAGMENT_SHADER, kBrokenFs);
        const GLuint program = CreateProgram();
        AttachShader(program, vs);
        AttachShader(program, fs);
        LinkProgram(program);
        ASSERT_EQ(QueryLinkStatus(program), GL_FALSE);
        syncLog = QueryProgramInfoLog(program);
        EXPECT_FALSE(syncLog.empty());
        EXPECT_EQ(GetError(), GL_NO_ERROR);
    }
    {
        const AsyncModeScope scope(true);
        const GLuint vs = MakeShader(GL_VERTEX_SHADER, kVs);
        const GLuint fs = MakeShader(GL_FRAGMENT_SHADER, kBrokenFs);
        const GLuint program = CreateProgram();
        AttachShader(program, vs);
        AttachShader(program, fs);
        LinkProgram(program);
        EXPECT_EQ(QueryLinkStatus(program), GL_FALSE);
        EXPECT_EQ(QueryProgramInfoLog(program), syncLog);
        EXPECT_EQ(GetError(), GL_NO_ERROR);
    }
}

// A program with nothing attached fails in the GL-thread prologue, before any job exists.
// That path has to reach the same info log in both modes.
TEST_F(AsyncLinkTest, LinkWithNoShadersFailsIdenticallyInBothModes) {
    String syncLog;
    for (const Bool async : {false, true}) {
        const AsyncModeScope scope(async);
        const GLuint program = CreateProgram();
        LinkProgram(program);
        EXPECT_EQ(QueryLinkStatus(program), GL_FALSE);
        const String log = QueryProgramInfoLog(program);
        EXPECT_FALSE(log.empty());
        if (!async) {
            syncLog = log;
        } else {
            EXPECT_EQ(log, syncLog);
        }
        EXPECT_TRUE(LinkIsSettled(program)) << "a prologue failure leaves no job pending";
    }
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// ---------------------------------------------------------------------------------------
// End to end
// ---------------------------------------------------------------------------------------

// The shape a shaderpack load actually has: compile N shaders, link M programs, read
// NOTHING until the end, then query everything. This is the only shape in which the pool has
// many compiles and many links in flight simultaneously, with the link jobs chained behind
// compile jobs that are themselves still queued.
TEST_F(AsyncLinkTest, PackShapedBurstCompilesLinksAndQueriesEverything) {
    const AsyncModeScope async(true);
    constexpr int kShaders = 24;
    constexpr int kPrograms = 24;

    Vector<String> sources;
    Vector<GLuint> vertexShaders;
    Vector<GLuint> fragmentShaders;
    for (int i = 0; i < kShaders; ++i) {
        vertexShaders.push_back(MakeShader(GL_VERTEX_SHADER, kVs));
        sources.push_back(MakeBulkySource(8000 + i));
        const char* text = sources.back().c_str();
        const GLuint fs = CreateShader(GL_FRAGMENT_SHADER);
        ShaderSource(fs, 1, &text, nullptr);
        CompileShader(fs);
        fragmentShaders.push_back(fs);
    }

    Vector<GLuint> programs;
    for (int i = 0; i < kPrograms; ++i) {
        const GLuint program = CreateProgram();
        AttachShader(program, vertexShaders[static_cast<SizeT>(i % kShaders)]);
        AttachShader(program, fragmentShaders[static_cast<SizeT>(i % kShaders)]);
        LinkProgram(program);
        programs.push_back(program);
    }

    for (int i = 0; i < kPrograms; ++i) {
        const GLuint program = programs[static_cast<SizeT>(i)];
        ASSERT_EQ(QueryLinkStatus(program), GL_TRUE) << "program " << i << ": " << QueryProgramInfoLog(program);
        EXPECT_GE(GetUniformLocation(program, "uColor"), 0) << "program " << i;
        EXPECT_GE(GetUniformLocation(program, ("uSeed" + std::to_string(8000 + i % kShaders)).c_str()), 0)
            << "program " << i;
        EXPECT_EQ(GetAttribLocation(program, "aPos"), 0) << "program " << i;
        EXPECT_EQ(SpirvDigest(program).size(), 2u) << "program " << i;
    }
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// The adversarial interleaving: link, query a previous one, re-source, re-link, delete, all
// with the pool busy. Nothing here asserts timing - what it hunts for is a missed join, a
// consumed-twice parse, or a use of an abandoned node, all of which surface as a wrong
// status, a missing uniform, or a crash.
TEST_F(AsyncLinkTest, StressLinkQueryRelinkDeleteInterleaved) {
    const AsyncModeScope async(true);
    constexpr int kRounds = 5;
    constexpr int kPerRound = 10;

    for (int round = 0; round < kRounds; ++round) {
        Vector<String> sources;
        Vector<GLuint> programs;
        Vector<GLuint> fragmentShaders;
        const GLuint vs = MakeShader(GL_VERTEX_SHADER, kVs);

        for (int i = 0; i < kPerRound; ++i) {
            sources.push_back(MakeBulkySource(round * 1000 + 300 + i));
            const char* text = sources.back().c_str();
            const GLuint fs = CreateShader(GL_FRAGMENT_SHADER);
            ShaderSource(fs, 1, &text, nullptr);
            CompileShader(fs);
            fragmentShaders.push_back(fs);

            const GLuint program = CreateProgram();
            AttachShader(program, vs);
            AttachShader(program, fs);
            LinkProgram(program);
            programs.push_back(program);

            // Query a PREVIOUS program while this one is still outstanding: the join has to
            // settle exactly the program asked about and no other.
            if (i > 0) {
                const GLuint earlier = programs[static_cast<SizeT>(i - 1)];
                EXPECT_EQ(QueryLinkStatus(earlier), GL_TRUE) << QueryProgramInfoLog(earlier);
            }
        }

        // Re-source half of them mid-flight and relink over the pending link.
        for (int i = 0; i < kPerRound; i += 2) {
            sources.push_back(MakeBulkySource(round * 1000 + 700 + i));
            const char* text = sources.back().c_str();
            ShaderSource(fragmentShaders[static_cast<SizeT>(i)], 1, &text, nullptr);
            CompileShader(fragmentShaders[static_cast<SizeT>(i)]);
            LinkProgram(programs[static_cast<SizeT>(i)]);
        }

        for (int i = 0; i < kPerRound; ++i) {
            const GLuint program = programs[static_cast<SizeT>(i)];
            ASSERT_EQ(QueryLinkStatus(program), GL_TRUE)
                << "round " << round << " program " << i << ": " << QueryProgramInfoLog(program);
            const String expected =
                "uSeed" + std::to_string(round * 1000 + (i % 2 == 0 ? 700 + i : 300 + i));
            EXPECT_GE(GetUniformLocation(program, expected.c_str()), 0)
                << "round " << round << " program " << i << " expected " << expected;
            DeleteProgram(program);
        }
        for (const GLuint fs : fragmentShaders) DeleteShader(fs);
        DeleteShader(vs);
        EXPECT_EQ(GetError(), GL_NO_ERROR);
    }
}
