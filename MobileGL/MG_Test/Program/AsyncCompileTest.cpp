// MobileGL - MobileGL/MG_Test/Program/AsyncCompileTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P1 stage 3: glCompileShader enqueues, and every observable read joins.
//
// Every test here drives the real GL entry points and flips
// MG_Config::Features.AsyncShaderCompile itself rather than reading the environment. That is
// what lets one binary assert the property that actually matters - the async path and the
// synchronous path are indistinguishable through the GL surface - and it makes the file
// behave identically whether or not the suite was launched with
// MOBILEGL_ASYNC_SHADER_COMPILE=1.

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <vector>

#include "Config.h"
#include "Includes.h"
#include "Init.h"
#include "MG_Impl/GLImpl/Getter/GL_Getter.h"
#include "MG_Impl/GLImpl/Program/GL_Program.h"
#include "MG_State/GLState/Core.h"
#include "MG_Util/Async/ShaderCompilePool.h"

using namespace MobileGL;
using namespace MobileGL::MG_Impl::GLImpl;

namespace {
    // Restores whatever the environment asked for when the test ends, so a case that forces
    // one mode cannot leak into the next.
    class AsyncModeScope {
    public:
        explicit AsyncModeScope(const Bool async)
            : m_saved(MG_Config::Features.AsyncShaderCompile) {
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

    // Fails in glslang, not in the lexical pre-checks: that routes through the same
    // ParseFailed path a real broken shaderpack source takes.
    const char* kBrokenFs = R"(#version 460
layout(location = 0) out vec4 fragColor;
void main() { fragColor = thisIdentifierWasNeverDeclared; }
)";

    // Rejected by the lexical reserved-identifier scan, before glslang is ever reached - the
    // other half of the "compile failed" surface, and the one that never allocates a parse.
    const char* kReservedIdentifierFs = R"(#version 460
layout(location = 0) out vec4 fragColor;
float gl_NotAllowedToDeclareThis = 1.0;
void main() { fragColor = vec4(gl_NotAllowedToDeclareThis); }
)";

    // Big enough that a compile is not instantaneous, so the pool actually has a backlog to
    // observe. Templated on an index so every instance is a distinct source (no P0b hit).
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
        return shader;
    }

    GLint QueryCompileStatus(const GLuint shader) {
        GLint status = GL_FALSE;
        GetShaderiv(shader, GL_COMPILE_STATUS, &status);
        return status;
    }

    String QueryShaderInfoLog(const GLuint shader) {
        GLint length = 0;
        GetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
        if (length <= 0) return String();
        std::vector<GLchar> buffer(static_cast<size_t>(length));
        GLsizei written = 0;
        GetShaderInfoLog(shader, length, &written, buffer.data());
        return String(buffer.data(), static_cast<size_t>(written));
    }

    GLint QueryLinkStatus(const GLuint program) {
        GLint status = GL_FALSE;
        GetProgramiv(program, GL_LINK_STATUS, &status);
        return status;
    }

    // The non-joining view of the object, i.e. what GL_COMPLETION_STATUS_KHR will report.
    Bool CompileIsSettled(const GLuint shader) {
        const auto& object = MG_State::pGLContext->GetShaderObject(shader);
        return object == nullptr || object->IsCompileComplete();
    }

    Bool HasMemoizedCompile(const GLuint shader) {
        const auto& object = MG_State::pGLContext->GetShaderObject(shader);
        return object != nullptr && object->HasMemoizedCompile();
    }

    // Enqueues `count` distinct heavy compiles and returns their names WITHOUT reading
    // anything back, so the pool is left with a real backlog for the caller to race against.
    Vector<GLuint> SaturatePool(const int count, Vector<String>& sourceStorage) {
        Vector<GLuint> shaders;
        shaders.reserve(static_cast<SizeT>(count));
        sourceStorage.reserve(sourceStorage.size() + static_cast<SizeT>(count));
        for (int i = 0; i < count; ++i) {
            sourceStorage.push_back(MakeBulkySource(1000 + i));
            const char* text = sourceStorage.back().c_str();
            const GLuint shader = CreateShader(GL_FRAGMENT_SHADER);
            ShaderSource(shader, 1, &text, nullptr);
            CompileShader(shader);
            shaders.push_back(shader);
        }
        return shaders;
    }

    class AsyncCompileTest : public ::testing::Test {
    protected:
        void SetUp() override { MobileGL::Initialize(); }
    };
} // namespace

// ---------------------------------------------------------------------------------------
// Correctness through the full GL surface
// ---------------------------------------------------------------------------------------

// N shaders compiled with the flag on: every status, every info log and every link has to
// come out the same as the synchronous path produces.
TEST_F(AsyncCompileTest, ManyShadersCompileAndLinkCorrectlyWithAsyncOn) {
    const AsyncModeScope async(true);
    ASSERT_TRUE(MG_Util::Async::AsyncShaderCompileEnabled());

    constexpr int kCount = 24;
    Vector<GLuint> vertexShaders;
    Vector<GLuint> fragmentShaders;
    Vector<String> sources;
    sources.reserve(kCount);

    // Enqueue everything first, read nothing: this is the shape a shaderpack load has, and
    // the only shape where the pool has more than one job in flight at a time.
    for (int i = 0; i < kCount; ++i) {
        vertexShaders.push_back(MakeShader(GL_VERTEX_SHADER, kVs));
        sources.push_back(MakeBulkySource(i));
        const char* text = sources.back().c_str();
        const GLuint fs = CreateShader(GL_FRAGMENT_SHADER);
        ShaderSource(fs, 1, &text, nullptr);
        CompileShader(fs);
        fragmentShaders.push_back(fs);
        CompileShader(vertexShaders.back());
    }

    for (int i = 0; i < kCount; ++i) {
        EXPECT_EQ(QueryCompileStatus(vertexShaders[i]), GL_TRUE) << QueryShaderInfoLog(vertexShaders[i]);
        EXPECT_EQ(QueryCompileStatus(fragmentShaders[i]), GL_TRUE) << QueryShaderInfoLog(fragmentShaders[i]);
        EXPECT_TRUE(QueryShaderInfoLog(vertexShaders[i]).empty());
        EXPECT_TRUE(QueryShaderInfoLog(fragmentShaders[i]).empty());
    }

    // And the artifacts are actually usable: link, and reflect a uniform out of each stage.
    for (int i = 0; i < kCount; ++i) {
        const GLuint program = CreateProgram();
        AttachShader(program, vertexShaders[i]);
        AttachShader(program, fragmentShaders[i]);
        LinkProgram(program);
        ASSERT_EQ(QueryLinkStatus(program), GL_TRUE) << "program " << i;
        EXPECT_GE(GetUniformLocation(program, "uColor"), 0);
        EXPECT_GE(GetUniformLocation(program, ("uSeed" + std::to_string(i)).c_str()), 0);
    }
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// glCompileShader must return before the work is done. Timing-based assertions flake, so
// this observes the state machine instead: with a saturated pool at least one of the just
// -enqueued shaders has to be unsettled at the moment we ask. Skipped rather than failed if
// the machine drained the whole batch first - it can then never be a false red.
TEST_F(AsyncCompileTest, CompileShaderReturnsBeforeTheWorkIsDone) {
    const AsyncModeScope async(true);
    Vector<String> sources;
    const Vector<GLuint> shaders = SaturatePool(64, sources);

    int unsettled = 0;
    for (const GLuint shader : shaders) {
        if (!CompileIsSettled(shader)) ++unsettled;
    }
    if (unsettled == 0) {
        GTEST_SKIP() << "the pool drained 64 compiles before the first observation; nothing to prove here";
    }

    // Whatever was outstanding still has to produce the right answer once asked.
    for (const GLuint shader : shaders) {
        EXPECT_EQ(QueryCompileStatus(shader), GL_TRUE) << QueryShaderInfoLog(shader);
        EXPECT_TRUE(CompileIsSettled(shader)) << "reading COMPILE_STATUS must have joined";
    }
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// The synchronous path must stay synchronous: with the flag off, a compile is finished by
// the time glCompileShader returns. This is the guard that keeps the default shippable.
TEST_F(AsyncCompileTest, CompileIsFullySynchronousWithAsyncOff) {
    const AsyncModeScope async(false);
    ASSERT_FALSE(MG_Util::Async::AsyncShaderCompileEnabled());

    Vector<String> sources;
    const Vector<GLuint> shaders = SaturatePool(8, sources);
    for (const GLuint shader : shaders) {
        EXPECT_TRUE(CompileIsSettled(shader));
        EXPECT_TRUE(HasMemoizedCompile(shader));
        EXPECT_EQ(QueryCompileStatus(shader), GL_TRUE) << QueryShaderInfoLog(shader);
    }
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// ---------------------------------------------------------------------------------------
// Diagnostics: the failing paths must read identically in both modes
// ---------------------------------------------------------------------------------------

// A compile failure is reported through COMPILE_STATUS and the info log, never through
// glGetError - that is exactly why moving the work off-thread is legal. Both failure
// classes are covered: the glslang parse failure and the lexical reserved-identifier
// rejection (which never reaches glslang at all).
TEST_F(AsyncCompileTest, FailingCompileLogIsByteIdenticalAcrossModes) {
    for (const char* source : {kBrokenFs, kReservedIdentifierFs}) {
        String syncLog;
        {
            const AsyncModeScope async(false);
            const GLuint fs = MakeShader(GL_FRAGMENT_SHADER, source);
            CompileShader(fs);
            ASSERT_EQ(QueryCompileStatus(fs), GL_FALSE);
            syncLog = QueryShaderInfoLog(fs);
            EXPECT_FALSE(syncLog.empty());
            // GL defines compile FAILURE as a status plus a log, not as a GL error.
            EXPECT_EQ(GetError(), GL_NO_ERROR);
        }
        {
            const AsyncModeScope async(true);
            const GLuint fs = MakeShader(GL_FRAGMENT_SHADER, source);
            CompileShader(fs);
            EXPECT_EQ(QueryCompileStatus(fs), GL_FALSE);
            EXPECT_EQ(QueryShaderInfoLog(fs), syncLog);
            EXPECT_EQ(GetError(), GL_NO_ERROR);
        }
    }
}

// A link whose vertex shader failed to compile has to reproduce that shader's log verbatim
// inside the program info log, whichever thread produced it.
TEST_F(AsyncCompileTest, LinkDiagnosticsQuoteTheAsyncCompileLog) {
    const AsyncModeScope async(true);
    const GLuint vs = MakeShader(GL_VERTEX_SHADER, kVs);
    const GLuint fs = MakeShader(GL_FRAGMENT_SHADER, kBrokenFs);
    CompileShader(vs);
    CompileShader(fs);

    const GLuint program = CreateProgram();
    AttachShader(program, vs);
    AttachShader(program, fs);
    // No status read between the enqueue and the link: the link's own prologue is what has
    // to join the two compiles.
    LinkProgram(program);
    EXPECT_EQ(QueryLinkStatus(program), GL_FALSE);

    GLint length = 0;
    GetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
    ASSERT_GT(length, 1);
    std::vector<GLchar> buffer(static_cast<size_t>(length));
    GLsizei written = 0;
    GetProgramInfoLog(program, length, &written, buffer.data());
    const String programLog(buffer.data(), static_cast<size_t>(written));
    const String shaderLog = QueryShaderInfoLog(fs);
    ASSERT_FALSE(shaderLog.empty());
    EXPECT_NE(programLog.find(shaderLog), String::npos)
        << "program log:\n" << programLog << "\nshader log:\n" << shaderLog;
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// ---------------------------------------------------------------------------------------
// Mutation over an in-flight compile
// ---------------------------------------------------------------------------------------

// glShaderSource with DIFFERENT text over a pending compile: the running job is abandoned
// and the next compile reflects the new source. The re-source happens with the pool
// saturated, so the job it replaces is very likely still queued or running.
TEST_F(AsyncCompileTest, ShaderSourceOverAPendingCompileCancelsAndTheNewSourceWins) {
    const AsyncModeScope async(true);
    Vector<String> backlog;
    SaturatePool(48, backlog);

    const String firstSource = MakeBulkySource(7001);
    const char* firstText = firstSource.c_str();
    const GLuint fs = CreateShader(GL_FRAGMENT_SHADER);
    ShaderSource(fs, 1, &firstText, nullptr);
    CompileShader(fs);

    // Replace the text while that compile is (very probably) still outstanding. This must
    // not wait, must not corrupt the abandoned job's view of the old string, and must
    // disarm the layer-1 memo.
    const String secondSource = MakeBulkySource(7002);
    const char* secondText = secondSource.c_str();
    ShaderSource(fs, 1, &secondText, nullptr);
    EXPECT_FALSE(HasMemoizedCompile(fs)) << "a real source change must invalidate the compiled state";
    EXPECT_EQ(QueryCompileStatus(fs), GL_FALSE) << "the replaced compile must not publish";

    CompileShader(fs);
    ASSERT_EQ(QueryCompileStatus(fs), GL_TRUE) << QueryShaderInfoLog(fs);

    const GLuint vs = MakeShader(GL_VERTEX_SHADER, kVs);
    CompileShader(vs);
    const GLuint program = CreateProgram();
    AttachShader(program, vs);
    AttachShader(program, fs);
    LinkProgram(program);
    ASSERT_EQ(QueryLinkStatus(program), GL_TRUE);
    // The SECOND source's uniform is the one that exists.
    EXPECT_GE(GetUniformLocation(program, "uSeed7002"), 0);
    EXPECT_EQ(GetUniformLocation(program, "uSeed7001"), -1);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// glShaderSource with byte-identical text over a pending compile is a no-op: the job stays,
// the memo stays armed, and the result is still the right one.
TEST_F(AsyncCompileTest, IdenticalShaderSourceOverAPendingCompileKeepsTheJob) {
    const AsyncModeScope async(true);
    Vector<String> backlog;
    SaturatePool(48, backlog);

    const String source = MakeBulkySource(7100);
    const char* text = source.c_str();
    const GLuint fs = CreateShader(GL_FRAGMENT_SHADER);
    ShaderSource(fs, 1, &text, nullptr);
    CompileShader(fs);

    ShaderSource(fs, 1, &text, nullptr);
    EXPECT_TRUE(HasMemoizedCompile(fs)) << "identical re-source must not disturb an in-flight compile";
    EXPECT_EQ(QueryCompileStatus(fs), GL_TRUE) << QueryShaderInfoLog(fs);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// A second glCompileShader on a pending object must be a no-op, not a duplicate job racing
// the first one to write the same fields. Observed through the object identity of the node:
// HasMemoizedCompile stays true across the second call, and the result is still correct.
TEST_F(AsyncCompileTest, RepeatedCompileShaderOnAPendingObjectEnqueuesOneJob) {
    const AsyncModeScope async(true);
    Vector<String> backlog;
    SaturatePool(48, backlog);

    const String source = MakeBulkySource(7200);
    const char* text = source.c_str();
    const GLuint fs = CreateShader(GL_FRAGMENT_SHADER);
    ShaderSource(fs, 1, &text, nullptr);

    // A copy, not the slot reference: creating another shader can reallocate the table.
    const SharedPtr<MG_State::GLState::ShaderObject> object = MG_State::pGLContext->GetShaderObject(fs);
    ASSERT_NE(object, nullptr);
    EXPECT_FALSE(object->HasMemoizedCompile());
    CompileShader(fs);
    EXPECT_TRUE(object->HasMemoizedCompile());
    for (int i = 0; i < 8; ++i) {
        CompileShader(fs);
        EXPECT_TRUE(object->HasMemoizedCompile());
    }

    EXPECT_EQ(QueryCompileStatus(fs), GL_TRUE) << QueryShaderInfoLog(fs);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// glDeleteShader on an unattached object with a compile still in flight. The name goes away
// immediately - no wait for a worker - and the abandoned job must neither crash nor keep the
// object alive in a way anything can observe.
TEST_F(AsyncCompileTest, DeleteShaderWhileACompileIsPending) {
    const AsyncModeScope async(true);
    Vector<String> backlog;
    SaturatePool(48, backlog);

    Vector<GLuint> doomed;
    Vector<String> sources;
    for (int i = 0; i < 16; ++i) {
        sources.push_back(MakeBulkySource(7300 + i));
        const char* text = sources.back().c_str();
        const GLuint fs = CreateShader(GL_FRAGMENT_SHADER);
        ShaderSource(fs, 1, &text, nullptr);
        CompileShader(fs);
        doomed.push_back(fs);
    }
    for (const GLuint fs : doomed) {
        DeleteShader(fs);
        EXPECT_EQ(IsShader(fs), GL_FALSE) << "an unattached deleted shader's name goes immediately";
    }
    EXPECT_EQ(GetError(), GL_NO_ERROR);

    // The context still works afterwards - the abandoned jobs did not take the pool, the
    // preprocess cache or the glslang process state down with them.
    const GLuint vs = MakeShader(GL_VERTEX_SHADER, kVs);
    const GLuint fs = MakeShader(GL_FRAGMENT_SHADER, kFs);
    CompileShader(vs);
    CompileShader(fs);
    const GLuint program = CreateProgram();
    AttachShader(program, vs);
    AttachShader(program, fs);
    LinkProgram(program);
    EXPECT_EQ(QueryLinkStatus(program), GL_TRUE);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// glDeleteShader on a shader still ATTACHED to a program only flags it: the pending compile
// has to survive, because the link that follows still needs its artifacts.
TEST_F(AsyncCompileTest, DeleteShaderWhileAttachedKeepsThePendingCompileAlive) {
    const AsyncModeScope async(true);
    Vector<String> backlog;
    SaturatePool(48, backlog);

    const GLuint vs = MakeShader(GL_VERTEX_SHADER, kVs);
    const GLuint fs = MakeShader(GL_FRAGMENT_SHADER, kFs);
    const GLuint program = CreateProgram();
    AttachShader(program, vs);
    AttachShader(program, fs);
    CompileShader(vs);
    CompileShader(fs);
    DeleteShader(vs);
    DeleteShader(fs);

    LinkProgram(program);
    ASSERT_EQ(QueryLinkStatus(program), GL_TRUE);
    EXPECT_GE(GetUniformLocation(program, "uColor"), 0);
    EXPECT_GE(GetUniformLocation(program, "uAlpha"), 0);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// ---------------------------------------------------------------------------------------
// Stress
// ---------------------------------------------------------------------------------------

// The adversarial interleaving: enqueue, query, re-source, re-enqueue, delete, all with the
// pool busy. Nothing here asserts timing - what it hunts for is a missed join or a use of an
// abandoned node, both of which surface as a wrong status, a wrong log, or a crash.
TEST_F(AsyncCompileTest, StressCompileQueryResourceDeleteInterleaved) {
    const AsyncModeScope async(true);
    constexpr int kRounds = 6;
    constexpr int kPerRound = 12;

    for (int round = 0; round < kRounds; ++round) {
        Vector<String> sources;
        Vector<GLuint> shaders;
        sources.reserve(kPerRound * 2);

        for (int i = 0; i < kPerRound; ++i) {
            sources.push_back(MakeBulkySource(round * 1000 + i));
            const char* text = sources.back().c_str();
            const GLuint fs = CreateShader(GL_FRAGMENT_SHADER);
            ShaderSource(fs, 1, &text, nullptr);
            CompileShader(fs);
            shaders.push_back(fs);

            // Immediately query a PREVIOUS one while this one is still outstanding: the
            // join has to settle exactly the object asked about and no other.
            if (i > 0) {
                const GLuint earlier = shaders[static_cast<SizeT>(i - 1)];
                EXPECT_EQ(QueryCompileStatus(earlier), GL_TRUE) << QueryShaderInfoLog(earlier);
            }
        }

        // Re-source half of them mid-flight, then recompile.
        for (int i = 0; i < kPerRound; i += 2) {
            sources.push_back(MakeBulkySource(round * 1000 + 500 + i));
            const char* text = sources.back().c_str();
            ShaderSource(shaders[static_cast<SizeT>(i)], 1, &text, nullptr);
            CompileShader(shaders[static_cast<SizeT>(i)]);
        }

        for (int i = 0; i < kPerRound; ++i) {
            const GLuint shader = shaders[static_cast<SizeT>(i)];
            EXPECT_EQ(QueryCompileStatus(shader), GL_TRUE) << QueryShaderInfoLog(shader);
            const String expectedUniform =
                "uSeed" + std::to_string(round * 1000 + (i % 2 == 0 ? 500 + i : i));
            const GLuint vs = MakeShader(GL_VERTEX_SHADER, kVs);
            CompileShader(vs);
            const GLuint program = CreateProgram();
            AttachShader(program, vs);
            AttachShader(program, shader);
            LinkProgram(program);
            ASSERT_EQ(QueryLinkStatus(program), GL_TRUE) << "round " << round << " shader " << i;
            EXPECT_GE(GetUniformLocation(program, expectedUniform.c_str()), 0)
                << "round " << round << " shader " << i << " expected " << expectedUniform;
            DeleteProgram(program);
            DeleteShader(vs);
        }
        for (const GLuint shader : shaders) {
            DeleteShader(shader);
        }
        EXPECT_EQ(GetError(), GL_NO_ERROR);
    }
}

// The P0b cross-object memo is hit from several workers at once here: 8 objects share each
// of 6 distinct sources, all enqueued before anything is read. Every object must still end
// up with its own parse and its own correct reflection - a torn cache entry or an entry
// evicted from under a reader shows up as a link failure or a missing uniform.
TEST_F(AsyncCompileTest, ConcurrentCompilesShareThePreprocessCacheSafely) {
    const AsyncModeScope async(true);
    constexpr int kDistinct = 6;
    constexpr int kDuplicates = 8;

    Vector<String> sources;
    sources.reserve(kDistinct);
    for (int i = 0; i < kDistinct; ++i) {
        sources.push_back(MakeBulkySource(8100 + i));
    }

    Vector<GLuint> shaders;
    for (int duplicate = 0; duplicate < kDuplicates; ++duplicate) {
        for (int i = 0; i < kDistinct; ++i) {
            const char* text = sources[static_cast<SizeT>(i)].c_str();
            const GLuint fs = CreateShader(GL_FRAGMENT_SHADER);
            ShaderSource(fs, 1, &text, nullptr);
            CompileShader(fs);
            shaders.push_back(fs);
        }
    }

    for (SizeT s = 0; s < shaders.size(); ++s) {
        const GLuint fs = shaders[s];
        ASSERT_EQ(QueryCompileStatus(fs), GL_TRUE) << QueryShaderInfoLog(fs);
        const GLuint vs = MakeShader(GL_VERTEX_SHADER, kVs);
        CompileShader(vs);
        const GLuint program = CreateProgram();
        AttachShader(program, vs);
        AttachShader(program, fs);
        LinkProgram(program);
        ASSERT_EQ(QueryLinkStatus(program), GL_TRUE) << "shader index " << s;
        const String uniform = "uSeed" + std::to_string(8100 + static_cast<int>(s % kDistinct));
        EXPECT_GE(GetUniformLocation(program, uniform.c_str()), 0) << uniform;
    }
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}
