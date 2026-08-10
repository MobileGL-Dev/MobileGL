// MobileGL - MobileGL/MG_Test/Program/OptimisticStatusTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// MOBILEGL_ASYNC_OPTIMISTIC_SHADER_STATUS: while a compile job is in flight, the two
// per-shader queries that would join it - GL_COMPILE_STATUS and the info log - answer
// optimistically instead, and the first such answer latches for that compile's lifetime
// (ShaderObject::TakeOptimisticCompileAnswer). These cases pin the corners of that
// contract: the default still joins, the optimistic window really answers without
// joining, the latch keeps the three queries telling one story even after the job
// settles, a real failure still fails the program link with the compile log quoted, and
// the Iris-shaped two-phase batch produces reflection identical to the joining path.
//
// Determinism note: the cases that need "a compile that cannot have settled yet" do not
// race the pool - they occupy its single concurrency slot with a gate-blocked job
// (PoolBlocker), so the assertions are hard EXPECTs rather than skip-if-drained guesses.
// A quirk that silently reverts to joining DEADLOCKS such a case into its 300s ctest
// timeout instead of passing - ugly, but a failure, which is the point.
//
// Like AsyncCompileTest, every case drives the real GL entry points and flips the
// MG_Config::Features fields itself rather than reading the environment, so one binary
// asserts both flag states regardless of how the suite was launched.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Config.h"
#include "Includes.h"
#include "Init.h"
#include "MG_Impl/GLImpl/Getter/GL_Getter.h"
#include "MG_Impl/GLImpl/Program/GL_Program.h"
#include "MG_State/GLState/Core.h"
#include "MG_Util/Async/JobNode.h"
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

    class OptimisticStatusScope {
    public:
        explicit OptimisticStatusScope(const MG_Config::QuirkOverride mode)
            : m_saved(MG_Config::Features.AsyncOptimisticShaderStatus) {
            MG_Config::Features.AsyncOptimisticShaderStatus = mode;
        }
        ~OptimisticStatusScope() { MG_Config::Features.AsyncOptimisticShaderStatus = m_saved; }
        OptimisticStatusScope(const OptimisticStatusScope&) = delete;
        OptimisticStatusScope& operator=(const OptimisticStatusScope&) = delete;

    private:
        const MG_Config::QuirkOverride m_saved;
    };

    // glMaxShaderCompilerThreadsKHR writes PROCESS-wide state (the pool's concurrency budget
    // and the suspension latch), so a case that touches it has to put both back or it
    // poisons every case declared after it in this binary.
    class CompilerThreadScope {
    public:
        CompilerThreadScope() = default;
        ~CompilerThreadScope() {
            MG_Util::Async::SetAsyncShaderCompileSuspended(false);
            MG_Util::Async::ShaderCompilePool::Get().SetMaxConcurrency(
                MG_Util::Async::ShaderCompilePool::Get().GetThreadCount());
        }
        CompilerThreadScope(const CompilerThreadScope&) = delete;
        CompilerThreadScope& operator=(const CompilerThreadScope&) = delete;
    };

    // A job that occupies a pool slot until released, holding everything queued behind it
    // in a provably-unsettled state. Same gate idea as JobNodeTest's TestJob+Gate; waiting
    // on a test-owned gate inside a body does not violate the pool's no-job-waits-on-job
    // rule - there is no other JOB involved.
    class PoolBlocker final : public MG_Util::Async::JobNode {
    public:
        void Release() {
            {
                const std::lock_guard<std::mutex> lock(m_mutex);
                m_open = true;
            }
            m_cv.notify_all();
        }

    protected:
        void RunBody() override {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this] { return m_open; });
        }

    private:
        std::mutex m_mutex;
        std::condition_variable m_cv;
        Bool m_open = false;
    };

    // Budget 1 + a blocked job in the only slot: from construction until Release(), no
    // shader compile posted afterwards can run, let alone settle. The destructor releases
    // and joins so no case can leak a wedged pool into the next one.
    class BlockedPoolScope {
    public:
        BlockedPoolScope() : m_blocker(MakeShared<PoolBlocker>()) {
            MaxShaderCompilerThreadsKHR(1);
            MG_Util::Async::ShaderCompilePool::Get().Post(m_blocker);
        }
        ~BlockedPoolScope() { Release(); }

        void Release() {
            m_blocker->Release();
            m_blocker->Wait();
        }

        BlockedPoolScope(const BlockedPoolScope&) = delete;
        BlockedPoolScope& operator=(const BlockedPoolScope&) = delete;

    private:
        SharedPtr<PoolBlocker> m_blocker;
    };

    const char* kBrokenFs = R"(#version 460
layout(location = 0) out vec4 fragColor;
void main() { fragColor = thisIdentifierWasNeverDeclared; }
)";

    // Expensive enough that a compile is not instantaneous, and distinct per index so the
    // source-hash memo and the stage-6 adoption map never turn a second instance into a
    // no-op. Callers pass disjoint seed ranges for the same reason - two calls in one case
    // must never regenerate the same text.
    String MakeBulkySource(const int index) {
        String source = "#version 460\nlayout(location = 0) out vec4 fragColor;\n";
        source += "uniform float uSeed" + std::to_string(index) + ";\n";
        source += "void main() {\n    float acc = uSeed" + std::to_string(index) + ";\n";
        for (int i = 0; i < 320; ++i) {
            source += "    acc = acc * 1.0001 + sin(acc + " + std::to_string(i) + ".0) * cos(acc);\n";
        }
        source += "    fragColor = vec4(acc, acc, acc, 1.0);\n}\n";
        return source;
    }

    // The two stages of one Iris-shaped program. Distinct per index (so nothing is memoized
    // across programs) but IDENTICAL between the quirk-off and quirk-on replays of the same
    // index, which is what makes the reflection comparison meaningful.
    String MakeIrisVs(const int index) {
        String source = "#version 460\nlayout(location = 0) in vec3 aPos;\n";
        source += "uniform mat4 uModel" + std::to_string(index) + ";\n";
        source += "uniform vec4 uTint;\nout vec4 vColor;\n";
        source += "void main() {\n    vColor = uTint;\n    gl_Position = uModel" + std::to_string(index) +
                  " * vec4(aPos, 1.0);\n}\n";
        return source;
    }
    String MakeIrisFs(const int index) {
        String source = "#version 460\nlayout(location = 0) out vec4 fragColor;\nin vec4 vColor;\n";
        source += "uniform float uSeed" + std::to_string(index) + ";\nuniform vec2 uOffset;\n";
        source += "void main() {\n    float acc = uSeed" + std::to_string(index) + " + uOffset.x;\n";
        for (int i = 0; i < 40; ++i) {
            source += "    acc = acc * 1.0001 + sin(acc + " + std::to_string(i) + ".0);\n";
        }
        source += "    fragColor = vColor + vec4(acc, uOffset.y, 0.0, 1.0);\n}\n";
        return source;
    }

    GLuint MakeShader(const GLenum type, const char* source) {
        const GLuint shader = CreateShader(type);
        ShaderSource(shader, 1, &source, nullptr);
        CompileShader(shader);
        return shader;
    }

    GLint QueryShaderCompletion(const GLuint shader) {
        GLint status = -1;
        GetShaderiv(shader, GL_COMPLETION_STATUS_KHR, &status);
        return status;
    }

    GLint QueryCompileStatus(const GLuint shader) {
        GLint status = GL_FALSE;
        GetShaderiv(shader, GL_COMPILE_STATUS, &status);
        return status;
    }

    GLint QueryInfoLogLength(const GLuint shader) {
        GLint length = -1;
        GetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
        return length;
    }

    String QueryShaderInfoLog(const GLuint shader) {
        std::vector<GLchar> buffer(65536);
        GLsizei written = 0;
        GetShaderInfoLog(shader, (GLsizei)buffer.size(), &written, buffer.data());
        return String(buffer.data(), static_cast<size_t>(written));
    }

    GLint QueryLinkStatus(const GLuint program) {
        GLint status = GL_FALSE;
        GetProgramiv(program, GL_LINK_STATUS, &status);
        return status;
    }

    GLint QueryProgramCompletion(const GLuint program) {
        GLint status = -1;
        GetProgramiv(program, GL_COMPLETION_STATUS_KHR, &status);
        return status;
    }

    String QueryProgramInfoLog(const GLuint program) {
        // Iris reads through an explicit 32768-byte buffer; mirror that cap so the
        // log-ordering contract is asserted through the same window the application has.
        std::vector<GLchar> buffer(32768);
        GLsizei written = 0;
        GetProgramInfoLog(program, (GLsizei)buffer.size(), &written, buffer.data());
        return String(buffer.data(), static_cast<size_t>(written));
    }

    // Enqueues `count` distinct heavy compiles without reading anything back. Seed bases
    // must be disjoint across calls within one case (see MakeBulkySource).
    Vector<GLuint> SaturatePool(const int count, const int seedBase, Vector<String>& sourceStorage) {
        Vector<GLuint> shaders;
        shaders.reserve(static_cast<SizeT>(count));
        for (int i = 0; i < count; ++i) {
            sourceStorage.push_back(MakeBulkySource(seedBase + i));
            const char* text = sourceStorage.back().c_str();
            const GLuint shader = CreateShader(GL_FRAGMENT_SHADER);
            ShaderSource(shader, 1, &text, nullptr);
            CompileShader(shader);
            shaders.push_back(shader);
        }
        return shaders;
    }

    // One program driven through Iris's exact phase-1 shape: create, source, compile, read
    // the info log then the compile status (GlShader.createShader's order), attach, bind an
    // attrib, link, detach, delete. NO program-level query of any kind.
    GLuint RunIrisPhaseOne(const String& vsSource, const String& fsSource) {
        const char* vsText = vsSource.c_str();
        const char* fsText = fsSource.c_str();

        const GLuint vs = CreateShader(GL_VERTEX_SHADER);
        ShaderSource(vs, 1, &vsText, nullptr);
        CompileShader(vs);
        (void)QueryShaderInfoLog(vs);
        (void)QueryCompileStatus(vs);

        const GLuint fs = CreateShader(GL_FRAGMENT_SHADER);
        ShaderSource(fs, 1, &fsText, nullptr);
        CompileShader(fs);
        (void)QueryShaderInfoLog(fs);
        (void)QueryCompileStatus(fs);

        const GLuint program = CreateProgram();
        AttachShader(program, vs);
        AttachShader(program, fs);
        BindAttribLocation(program, 0, "aPos");
        LinkProgram(program);
        DetachShader(program, vs);
        DetachShader(program, fs);
        DeleteShader(vs);
        DeleteShader(fs);
        return program;
    }

    // Phase 2, also in Iris's order: LINK_STATUS first, then the by-name location lookups,
    // then the GL_ACTIVE_UNIFORMS enumeration ProgramUniforms$Builder.buildUniforms does.
    struct ProgramReflection {
        GLint linkStatus = GL_FALSE;
        Vector<std::pair<String, GLint>> locations;                       // queried name -> location
        Vector<std::tuple<String, GLenum, GLint, GLint>> activeUniforms;  // name, type, size, location
    };

    ProgramReflection RunIrisPhaseTwo(const GLuint program, const Vector<String>& names) {
        ProgramReflection out;
        out.linkStatus = QueryLinkStatus(program);

        for (const String& name : names) {
            out.locations.emplace_back(name, GetUniformLocation(program, name.c_str()));
        }

        GLint activeCount = 0;
        GetProgramiv(program, GL_ACTIVE_UNIFORMS, &activeCount);
        for (GLint i = 0; i < activeCount; ++i) {
            GLchar name[128] = {};
            GLsizei written = 0;
            GLint size = 0;
            GLenum type = 0;
            GetActiveUniform(program, (GLuint)i, (GLsizei)sizeof(name), &written, &size, &type, name);
            const String nameStr(name, static_cast<size_t>(written));
            out.activeUniforms.emplace_back(nameStr, type, size, GetUniformLocation(program, name));
        }
        // The enumeration order is an implementation detail; the SET is the contract.
        std::sort(out.activeUniforms.begin(), out.activeUniforms.end());
        return out;
    }

    class OptimisticStatusTest : public ::testing::Test {
    protected:
        void SetUp() override { MobileGL::Initialize(); }
    };
} // namespace

// ---------------------------------------------------------------------------------------
// The default still joins
// ---------------------------------------------------------------------------------------

// With the quirk unset (Auto = the shipped default), GL_COMPILE_STATUS on a pending compile
// must join it: after the query, the node is terminal. This is the case that guards the
// default against ever silently flipping. No blocker here - a blocked pool would turn the
// (correct) joining behaviour into a deadlock; a plain backlog only makes the pre-join
// state likely, and the assertion is valid either way.
TEST_F(OptimisticStatusTest, OffByDefaultTheStatusStillJoins) {
    const AsyncModeScope async(true);
    const OptimisticStatusScope quirk(MG_Config::QuirkOverride::Auto);
    const CompilerThreadScope threads;
    MaxShaderCompilerThreadsKHR(1);

    Vector<String> backlog;
    const Vector<GLuint> saturation = SaturatePool(8, 70000, backlog);
    const Vector<GLuint> probes = SaturatePool(1, 71000, backlog);
    const GLuint probe = probes[0];

    EXPECT_EQ(QueryCompileStatus(probe), GL_TRUE);
    EXPECT_EQ(QueryShaderCompletion(probe), GL_TRUE)
        << "GL_COMPILE_STATUS with the quirk off must have joined the job";

    for (const GLuint shader : saturation) DeleteShader(shader);
    DeleteShader(probe);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// ---------------------------------------------------------------------------------------
// The optimistic window, deterministically
// ---------------------------------------------------------------------------------------

// A compile that provably cannot have settled (the pool's only slot is gate-blocked)
// answers GL_TRUE / length 0 / empty log, and GL_COMPLETION_STATUS_KHR still reads
// GL_FALSE after all three - i.e. none of them joined. Hard EXPECTs, no skip: if the
// quirk silently reverts to joining, the status read deadlocks against the blocked pool
// and the case fails by timeout.
TEST_F(OptimisticStatusTest, PendingCompileReportsTrueAndEmptyLogWithoutJoining) {
    const AsyncModeScope async(true);
    const OptimisticStatusScope quirk(MG_Config::QuirkOverride::ForceOn);
    const CompilerThreadScope threads;
    const BlockedPoolScope blocked;

    Vector<String> storage;
    const Vector<GLuint> probes = SaturatePool(1, 72000, storage);
    const GLuint probe = probes[0];

    EXPECT_EQ(QueryCompileStatus(probe), GL_TRUE) << "an in-flight compile must answer GL_TRUE";
    EXPECT_EQ(QueryInfoLogLength(probe), 0) << "an in-flight compile must answer an empty log length";
    EXPECT_TRUE(QueryShaderInfoLog(probe).empty()) << "an in-flight compile must answer an empty log";
    EXPECT_EQ(QueryShaderCompletion(probe), GL_FALSE)
        << "the three reads above must not have joined the blocked job";

    DeleteShader(probe);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// ---------------------------------------------------------------------------------------
// The latch: one story per compile
// ---------------------------------------------------------------------------------------

// The torn-pair regression case. A broken shader's log and status are read while the job
// is provably in flight (optimistic empty/GL_TRUE), the job then settles, and the app
// re-reads: the latch must keep the answers optimistic - GL_TRUE, empty log - rather than
// flip to the real GL_FALSE next to the already-consumed empty log. The real failure then
// surfaces at the link, with the compile error inside the application's 32768-byte read
// window (the compile log leads the quoted source in ConsumeShaders' format).
TEST_F(OptimisticStatusTest, LatchKeepsOneStoryPerCompileAndTheLinkCarriesTheDiagnostic) {
    const AsyncModeScope async(true);
    const OptimisticStatusScope quirk(MG_Config::QuirkOverride::ForceOn);
    const CompilerThreadScope threads;

    const GLuint vs = CreateShader(GL_VERTEX_SHADER);
    const char* vsText =
        "#version 460\nlayout(location = 0) in vec3 aPos;\nvoid main() { gl_Position = vec4(aPos, 1.0); }\n";
    ShaderSource(vs, 1, &vsText, nullptr);

    GLuint fs = 0;
    {
        const BlockedPoolScope blocked;
        CompileShader(vs);
        fs = MakeShader(GL_FRAGMENT_SHADER, kBrokenFs);

        // Iris's order, while nothing can settle: log (empty), then status (GL_TRUE).
        EXPECT_TRUE(QueryShaderInfoLog(fs).empty());
        EXPECT_EQ(QueryCompileStatus(fs), GL_TRUE);
        EXPECT_EQ(QueryShaderCompletion(fs), GL_FALSE);
    } // blocker released and joined; the broken compile can now settle

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (QueryShaderCompletion(fs) == GL_FALSE) {
        ASSERT_LT(std::chrono::steady_clock::now(), deadline) << "compile job never settled";
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Settled - but this shader already told the optimistic story, so it keeps telling it.
    EXPECT_EQ(QueryCompileStatus(fs), GL_TRUE)
        << "the latch must keep a queried-while-pending compile optimistic after it settles";
    EXPECT_EQ(QueryInfoLogLength(fs), 0);
    EXPECT_TRUE(QueryShaderInfoLog(fs).empty());

    // The truth arrives where the design routes it: at the link.
    const GLuint program = CreateProgram();
    AttachShader(program, vs);
    AttachShader(program, fs);
    LinkProgram(program);
    EXPECT_EQ(QueryLinkStatus(program), GL_FALSE) << "a latched-over failure must still fail the link";
    EXPECT_NE(QueryProgramInfoLog(program).find("thisIdentifierWasNeverDeclared"), String::npos)
        << "the compile error must lead the program info log, inside a 32768-byte window";

    DeleteProgram(program);
    DeleteShader(vs);
    DeleteShader(fs);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// A shader whose FIRST query arrives after the job settled was never answered
// optimistically, so it owes no continuity: the truth comes straight back. (The
// completion poll does not engage the latch - it is the extension's own non-joining
// query and always tells the truth.)
TEST_F(OptimisticStatusTest, OnceTerminalAnUnqueriedShaderTellsTheTruth) {
    const AsyncModeScope async(true);
    const OptimisticStatusScope quirk(MG_Config::QuirkOverride::ForceOn);

    const GLuint fs = MakeShader(GL_FRAGMENT_SHADER, kBrokenFs);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (QueryShaderCompletion(fs) == GL_FALSE) {
        ASSERT_LT(std::chrono::steady_clock::now(), deadline) << "compile job never settled";
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    EXPECT_EQ(QueryCompileStatus(fs), GL_FALSE) << "no optimistic answer was given, so no latch holds";
    EXPECT_GT(QueryInfoLogLength(fs), 0);
    EXPECT_NE(QueryShaderInfoLog(fs).find("thisIdentifierWasNeverDeclared"), String::npos);
    DeleteShader(fs);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// Recompiling resets the story: a latched optimistic answer must not survive a source
// change (the latch clears when the node changes hands or goes away).
TEST_F(OptimisticStatusTest, ANewCompileResetsTheLatch) {
    const AsyncModeScope async(true);
    const OptimisticStatusScope quirk(MG_Config::QuirkOverride::ForceOn);
    const CompilerThreadScope threads;

    GLuint fs = 0;
    {
        const BlockedPoolScope blocked;
        fs = MakeShader(GL_FRAGMENT_SHADER, kBrokenFs);
        EXPECT_EQ(QueryCompileStatus(fs), GL_TRUE); // latches
    }

    // New source, new compile, no query before it settles.
    const char* goodFs = "#version 460\nlayout(location = 0) out vec4 fragColor;\n"
                         "void main() { fragColor = vec4(1.0); }\n";
    ShaderSource(fs, 1, &goodFs, nullptr);
    CompileShader(fs);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (QueryShaderCompletion(fs) == GL_FALSE) {
        ASSERT_LT(std::chrono::steady_clock::now(), deadline) << "recompile never settled";
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_EQ(QueryCompileStatus(fs), GL_TRUE);
    EXPECT_TRUE(QueryShaderInfoLog(fs).empty());
    DeleteShader(fs);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// ---------------------------------------------------------------------------------------
// Failure still fails, at the link, inside the application's read window
// ---------------------------------------------------------------------------------------

// A broken fragment shader whose compile status was answered optimistically still fails
// its program link, and the compile error is readable through a 32768-byte
// glGetProgramInfoLog - the compile log LEADS the quoted source in ConsumeShaders'
// format, so even this >32KB shader source cannot push it out of the window.
TEST_F(OptimisticStatusTest, AFailingCompileStillFailsItsLink) {
    const AsyncModeScope async(true);
    const OptimisticStatusScope quirk(MG_Config::QuirkOverride::ForceOn);

    // A >32KB broken fragment shader: the undeclared identifier sits at the top, then bulk.
    String brokenSource = "#version 460\nlayout(location = 0) out vec4 fragColor;\n";
    brokenSource += "void main() {\n    float acc = thisIdentifierWasNeverDeclared;\n";
    for (int i = 0; i < 900; ++i) {
        brokenSource += "    acc = acc * 1.0001 + sin(acc + " + std::to_string(i) + ".0) * cos(acc);\n";
    }
    brokenSource += "    fragColor = vec4(acc);\n}\n";
    ASSERT_GT(brokenSource.size(), 32768u);

    const GLuint vs = MakeShader(GL_VERTEX_SHADER,
                                 "#version 460\nlayout(location = 0) in vec3 aPos;\n"
                                 "void main() { gl_Position = vec4(aPos, 1.0); }\n");
    const char* brokenText = brokenSource.c_str();
    const GLuint fs = CreateShader(GL_FRAGMENT_SHADER);
    ShaderSource(fs, 1, &brokenText, nullptr);
    CompileShader(fs);
    (void)QueryShaderInfoLog(fs);
    (void)QueryCompileStatus(fs); // may latch optimistic GL_TRUE; must not matter

    const GLuint program = CreateProgram();
    AttachShader(program, vs);
    AttachShader(program, fs);
    LinkProgram(program);

    EXPECT_EQ(QueryLinkStatus(program), GL_FALSE) << "a hidden compile failure must still fail the link";
    const String log = QueryProgramInfoLog(program);
    EXPECT_NE(log.find("thisIdentifierWasNeverDeclared"), String::npos)
        << "the compile error must be readable through a 32768-byte program info log window";

    DeleteProgram(program);
    DeleteShader(vs);
    DeleteShader(fs);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// ---------------------------------------------------------------------------------------
// The Iris two-phase replay
// ---------------------------------------------------------------------------------------

// THE LOAD-BEARING CASE. 24 programs through Iris's exact phase-1 shape (compile, read log
// then status per shader, link, detach, delete - no program query), then phase 2 (link
// status, by-name locations including an absent name, the active-uniform enumeration).
// Every location and every active-uniform record must equal what the identical sequence
// produces with the quirk off.
//
// Two determinism guards make this a real A/B rather than a tautology:
//   * The quirk-on arm runs FIRST, against a cold preprocess cache, and the reference arm
//     second - so it is the path under test that pays the full pipeline, not the control.
//   * The quirk-on arm's phase 1 runs over a BLOCKED pool, and every program is then
//     WITNESSED still-incomplete (GL_COMPLETION_STATUS_KHR == GL_FALSE) before the pool
//     is released: proof that no phase-1 call joined, i.e. the quirk was really engaged.
//     A quirk that silently reverts to joining deadlocks here and fails by timeout.
TEST_F(OptimisticStatusTest, IrisTwoPhaseReplayProducesIdenticalReflection) {
    constexpr int kPrograms = 24;

    Vector<ProgramReflection> reference;
    Vector<ProgramReflection> optimistic;

    for (const Bool quirkOn : {true, false}) {
        const AsyncModeScope async(true);
        const OptimisticStatusScope quirk(quirkOn ? MG_Config::QuirkOverride::ForceOn
                                                  : MG_Config::QuirkOverride::ForceOff);
        const CompilerThreadScope threads;

        Vector<String> vsSources, fsSources;
        for (int i = 0; i < kPrograms; ++i) {
            vsSources.push_back(MakeIrisVs(i));
            fsSources.push_back(MakeIrisFs(i));
        }

        Vector<GLuint> programs;
        if (quirkOn) {
            const BlockedPoolScope blocked;
            for (int i = 0; i < kPrograms; ++i) {
                programs.push_back(RunIrisPhaseOne(vsSources[(SizeT)i], fsSources[(SizeT)i]));
            }
            // The witness: phase 1 finished with the pool blocked, so nothing can have
            // settled and nothing can have been joined - every link must still be pending.
            for (int i = 0; i < kPrograms; ++i) {
                ASSERT_EQ(QueryProgramCompletion(programs[(SizeT)i]), GL_FALSE)
                    << "program " << i << " settled under a blocked pool - a phase-1 call must have joined";
            }
        } else {
            for (int i = 0; i < kPrograms; ++i) {
                programs.push_back(RunIrisPhaseOne(vsSources[(SizeT)i], fsSources[(SizeT)i]));
            }
        }

        Vector<ProgramReflection>& out = quirkOn ? optimistic : reference;
        for (int i = 0; i < kPrograms; ++i) {
            const Vector<String> names = {"uModel" + std::to_string(i), "uTint",
                                          "uSeed" + std::to_string(i), "uOffset", "uDoesNotExist"};
            out.push_back(RunIrisPhaseTwo(programs[(SizeT)i], names));
        }
        for (const GLuint program : programs) DeleteProgram(program);
        ASSERT_EQ(GetError(), GL_NO_ERROR);
    }

    ASSERT_EQ(reference.size(), optimistic.size());
    for (SizeT i = 0; i < reference.size(); ++i) {
        EXPECT_EQ(reference[i].linkStatus, GL_TRUE) << "program " << i;
        EXPECT_EQ(optimistic[i].linkStatus, GL_TRUE) << "program " << i;
        EXPECT_EQ(reference[i].locations, optimistic[i].locations)
            << "program " << i << ": by-name locations diverged under the quirk";
        EXPECT_EQ(reference[i].activeUniforms, optimistic[i].activeUniforms)
            << "program " << i << ": active-uniform enumeration diverged under the quirk";
        // The absent name answers -1 in both worlds.
        EXPECT_EQ(reference[i].locations.back().second, -1) << "program " << i;
    }
}

// ---------------------------------------------------------------------------------------
// The concurrency observable
// ---------------------------------------------------------------------------------------

// The crisp A/B that phase 1 stopped joining. Quirk-on arm: the phase-1 shape over a
// blocked pool completes without joining anything - every shader is then provably still
// in flight (hard EXPECT; an inert quirk deadlocks and fails by timeout). Quirk-off arm:
// the same shape joins at every status read, so nothing is left in flight afterwards.
TEST_F(OptimisticStatusTest, PhaseOneIssuesNoCompileJoin) {
    const AsyncModeScope async(true);
    const CompilerThreadScope threads;

    // Quirk on: nothing settles, nothing joins.
    {
        const OptimisticStatusScope quirk(MG_Config::QuirkOverride::ForceOn);
        const BlockedPoolScope blocked;
        Vector<String> storage;
        Vector<GLuint> shaders;
        for (int i = 0; i < 12; ++i) {
            storage.push_back(MakeBulkySource(90000 + i));
            const char* text = storage.back().c_str();
            const GLuint fs = CreateShader(GL_FRAGMENT_SHADER);
            ShaderSource(fs, 1, &text, nullptr);
            CompileShader(fs);
            (void)QueryShaderInfoLog(fs);
            (void)QueryCompileStatus(fs);
            shaders.push_back(fs);
        }
        for (const GLuint shader : shaders) {
            EXPECT_EQ(QueryShaderCompletion(shader), GL_FALSE)
                << "a phase-1 read joined a compile the blocked pool could not have run";
        }
        for (const GLuint shader : shaders) DeleteShader(shader);
    }

    // Quirk off: every status read joins its shader.
    {
        const OptimisticStatusScope quirk(MG_Config::QuirkOverride::ForceOff);
        MaxShaderCompilerThreadsKHR(1);
        Vector<String> storage;
        Vector<GLuint> shaders;
        for (int i = 0; i < 12; ++i) {
            storage.push_back(MakeBulkySource(80000 + i));
            const char* text = storage.back().c_str();
            const GLuint fs = CreateShader(GL_FRAGMENT_SHADER);
            ShaderSource(fs, 1, &text, nullptr);
            CompileShader(fs);
            (void)QueryShaderInfoLog(fs);
            (void)QueryCompileStatus(fs);
            shaders.push_back(fs);
        }
        for (const GLuint shader : shaders) {
            EXPECT_EQ(QueryShaderCompletion(shader), GL_TRUE)
                << "with the quirk off every per-shader status read must have joined";
        }
        for (const GLuint shader : shaders) DeleteShader(shader);
    }
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}
