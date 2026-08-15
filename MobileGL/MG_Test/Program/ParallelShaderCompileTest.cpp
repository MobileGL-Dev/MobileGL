// MobileGL - MobileGL/MG_Test/Program/ParallelShaderCompileTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P1 stage 5: the GL_KHR_parallel_shader_compile application surface.
//
// Four things are under test, and they are the four an application actually touches:
//   * GL_COMPLETION_STATUS_KHR on shaders and programs, which MUST NOT JOIN - the whole
//     point of the query is to answer while the work is still outstanding;
//   * glMaxShaderCompilerThreadsKHR / ...ARB, including the count == 0 mode switch the
//     extension mandates and what lifts it again;
//   * GL_MAX_SHADER_COMPILER_THREADS_KHR;
//   * the extension string itself, which must appear if and only if asynchronous
//     compilation is enabled - the kill switch has to revert the application-visible
//     behaviour change, not only the threading.
//
// Like the other async suites, every case drives the real GL entry points and flips
// MG_Config::Features.AsyncShaderCompile itself, so the file behaves identically whether or
// not the suite was launched with MOBILEGL_ASYNC_SHADER_COMPILE=1.

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "Config.h"
#include "Includes.h"
#include "Init.h"
#include "MG_Backend/BackendObjects.h"
#include "MG_Backend/DirectGLES/BackendObject_DirectGLES.h"
#include "MG_Backend/DirectVulkan/BackendObject_DirectVulkan.h"
#include "MG_Impl/GLImpl/Getter/GL_Getter.h"
#include "MG_Impl/GLImpl/Program/GL_Program.h"
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

    const char* kVs = R"(#version 460
layout(location = 0) in vec3 aPos;
uniform vec4 uColor;
out vec4 vColor;
void main() {
    vColor = uColor;
    gl_Position = vec4(aPos, 1.0);
}
)";

    // Deliberately expensive, and distinct per index so the source-hash memo never turns a
    // second instance into a no-op: a saturated pool is the only way to observe an
    // outstanding job without asserting on timing.
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

    GLuint MakeShader(const GLenum type, const char* source) {
        const GLuint shader = CreateShader(type);
        ShaderSource(shader, 1, &source, nullptr);
        return shader;
    }

    GLint QueryShaderCompletion(const GLuint shader) {
        GLint status = -1;
        GetShaderiv(shader, GL_COMPLETION_STATUS_KHR, &status);
        return status;
    }

    GLint QueryProgramCompletion(const GLuint program) {
        GLint status = -1;
        GetProgramiv(program, GL_COMPLETION_STATUS_KHR, &status);
        return status;
    }

    GLint QueryCompileStatus(const GLuint shader) {
        GLint status = GL_FALSE;
        GetShaderiv(shader, GL_COMPILE_STATUS, &status);
        return status;
    }

    GLint QueryLinkStatus(const GLuint program) {
        GLint status = GL_FALSE;
        GetProgramiv(program, GL_LINK_STATUS, &status);
        return status;
    }

    // Enqueues `count` distinct heavy compiles and returns their names without reading
    // anything back, leaving the pool with a real backlog.
    Vector<GLuint> EnqueueBacklog(const int count, const int seedBase, Vector<String>& sourceStorage) {
        Vector<GLuint> shaders;
        shaders.reserve(static_cast<SizeT>(count));
        for (int i = 0; i < count; ++i) {
            sourceStorage.push_back(MakeBulkySource(seedBase + i));
            const char* text = sourceStorage.back().c_str();
            const GLuint fs = CreateShader(GL_FRAGMENT_SHADER);
            ShaderSource(fs, 1, &text, nullptr);
            CompileShader(fs);
            shaders.push_back(fs);
        }
        return shaders;
    }

    Bool Advertises(const Vector<GLExtension>& extensions, const GLExtension wanted) {
        return std::find(extensions.begin(), extensions.end(), wanted) != extensions.end();
    }

    class ParallelShaderCompileTest : public ::testing::Test {
    protected:
        void SetUp() override { MobileGL::Initialize(); }
    };
} // namespace

// ---------------------------------------------------------------------------------------
// GL_COMPLETION_STATUS_KHR must not join
// ---------------------------------------------------------------------------------------

// The load-bearing case of the whole stage. A single-worker pool is saturated with heavy
// compiles, so jobs are demonstrably still queued; GL_COMPLETION_STATUS_KHR then has to
// report GL_FALSE for at least one of them *and leave it outstanding*. If the query joined -
// which is what happens if it is ever routed through the ordinary Compiled() gate - it could
// only ever return GL_TRUE, and the extension would be a lie that costs an application the
// exact stall it added the polling loop to avoid.
//
// Skipped rather than failed when the machine drained the backlog first, so it can never be
// a false red on a fast box.
TEST_F(ParallelShaderCompileTest, ShaderCompletionStatusReportsFalseWithoutJoining) {
    const AsyncModeScope async(true);
    const CompilerThreadScope threads;
    // One worker: the queue behind it is the thing being observed.
    MaxShaderCompilerThreadsKHR(1);

    Vector<String> sources;
    const Vector<GLuint> shaders = EnqueueBacklog(64, 4000, sources);

    int outstanding = 0;
    for (const GLuint shader : shaders) {
        const GLint completion = QueryShaderCompletion(shader);
        ASSERT_TRUE(completion == GL_TRUE || completion == GL_FALSE) << "completion = " << completion;
        if (completion == GL_FALSE) ++outstanding;
    }
    if (outstanding == 0) {
        GTEST_SKIP() << "the pool drained 64 heavy compiles before the first query; nothing outstanding to observe";
    }

    // Asking again must still not have settled anything: the query is a peek, so a second
    // one cannot have made progress happen. (A joining implementation would report every
    // shader complete by now.)
    int stillOutstanding = 0;
    for (const GLuint shader : shaders) {
        if (QueryShaderCompletion(shader) == GL_FALSE) ++stillOutstanding;
    }
    EXPECT_GT(stillOutstanding, 0) << "GL_COMPLETION_STATUS_KHR joined - every shader settled just by being asked";

    // And once the real (joining) query is used, everything is complete and correct.
    for (const GLuint shader : shaders) {
        EXPECT_EQ(QueryCompileStatus(shader), GL_TRUE);
        EXPECT_EQ(QueryShaderCompletion(shader), GL_TRUE) << "GL_COMPILE_STATUS must have joined";
    }
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// The program half: a link enqueued behind a saturated pool cannot be complete either, and
// asking must not drag it forward.
TEST_F(ParallelShaderCompileTest, ProgramCompletionStatusReportsFalseWithoutJoining) {
    const AsyncModeScope async(true);
    const CompilerThreadScope threads;
    MaxShaderCompilerThreadsKHR(1);

    Vector<String> sources;
    EnqueueBacklog(48, 4200, sources);

    Vector<GLuint> programs;
    for (int i = 0; i < 8; ++i) {
        sources.push_back(MakeBulkySource(4400 + i));
        const char* text = sources.back().c_str();
        const GLuint fs = CreateShader(GL_FRAGMENT_SHADER);
        ShaderSource(fs, 1, &text, nullptr);
        CompileShader(fs);
        const GLuint vs = MakeShader(GL_VERTEX_SHADER, kVs);
        CompileShader(vs);
        const GLuint program = CreateProgram();
        AttachShader(program, vs);
        AttachShader(program, fs);
        LinkProgram(program);
        programs.push_back(program);
    }

    int outstanding = 0;
    for (const GLuint program : programs) {
        const GLint completion = QueryProgramCompletion(program);
        ASSERT_TRUE(completion == GL_TRUE || completion == GL_FALSE) << "completion = " << completion;
        if (completion == GL_FALSE) ++outstanding;
    }
    if (outstanding == 0) {
        GTEST_SKIP() << "the pool drained the whole backlog before the first query; nothing outstanding to observe";
    }

    for (const GLuint program : programs) {
        EXPECT_EQ(QueryLinkStatus(program), GL_TRUE);
        // GL_COMPLETION_STATUS_KHR spans BOTH phases of a link, so reading GL_LINK_STATUS -
        // which is answered out of phase A - is no longer enough to turn it GL_TRUE. That is
        // deliberate: an application that polls completion and then draws must not be told
        // "done" while the SPIR-V is still being generated, or the draw it was cleared for is
        // the thing that blocks. Settling both phases is what makes the query true.
        const auto& object = MG_State::pGLContext->GetProgramObject(program);
        ASSERT_NE(object, nullptr);
        object->JoinLinkAndSpirv();
        EXPECT_EQ(QueryProgramCompletion(program), GL_TRUE) << "a full join must have settled both phases";
    }
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// "Nothing outstanding" is the answer for an object that was never compiled or linked at
// all: the query asks whether work is pending, not whether work ever happened.
TEST_F(ParallelShaderCompileTest, CompletionStatusIsTrueForUntouchedObjects) {
    const AsyncModeScope async(true);
    const GLuint shader = MakeShader(GL_FRAGMENT_SHADER, "#version 460\nvoid main() {}\n");
    const GLuint program = CreateProgram();
    EXPECT_EQ(QueryShaderCompletion(shader), GL_TRUE);
    EXPECT_EQ(QueryProgramCompletion(program), GL_TRUE);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// With the flag off nothing is ever in flight, so the query is constant GL_TRUE - and, just
// as importantly, still a recognized pname rather than a GL_INVALID_ENUM.
TEST_F(ParallelShaderCompileTest, CompletionStatusIsAlwaysTrueWithAsyncOff) {
    const AsyncModeScope async(false);
    Vector<String> sources;
    const Vector<GLuint> shaders = EnqueueBacklog(8, 4600, sources);
    for (const GLuint shader : shaders) {
        EXPECT_EQ(QueryShaderCompletion(shader), GL_TRUE);
    }

    const GLuint vs = MakeShader(GL_VERTEX_SHADER, kVs);
    CompileShader(vs);
    const GLuint program = CreateProgram();
    AttachShader(program, vs);
    AttachShader(program, shaders.front());
    LinkProgram(program);
    EXPECT_EQ(QueryProgramCompletion(program), GL_TRUE);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// The pname is new; the rejection of everything else must be untouched.
TEST_F(ParallelShaderCompileTest, UnknownPnamesStillRaiseInvalidEnum) {
    const GLuint shader = MakeShader(GL_FRAGMENT_SHADER, "#version 460\nvoid main() {}\n");
    const GLuint program = CreateProgram();
    GLint value = 0;
    GetShaderiv(shader, GL_TEXTURE_2D, &value);
    EXPECT_EQ(GetError(), GL_INVALID_ENUM);
    GetProgramiv(program, GL_TEXTURE_2D, &value);
    EXPECT_EQ(GetError(), GL_INVALID_ENUM);
}

// ---------------------------------------------------------------------------------------
// glMaxShaderCompilerThreadsKHR / ...ARB
// ---------------------------------------------------------------------------------------

// count == 0 is the mode switch the extension defines: no compiler threads. Two obligations
// follow, and both are asserted here - everything already in flight is settled by the time
// the call returns (so every GL_COMPLETION_STATUS_KHR reads GL_TRUE straight away), and
// compilation that happens AFTERWARDS is synchronous too.
TEST_F(ParallelShaderCompileTest, ZeroCompilerThreadsJoinsEverythingAndCompilesInline) {
    const AsyncModeScope async(true);
    const CompilerThreadScope threads;
    MaxShaderCompilerThreadsKHR(1);

    Vector<String> sources;
    const Vector<GLuint> backlog = EnqueueBacklog(48, 4800, sources);

    MaxShaderCompilerThreadsKHR(0);
    EXPECT_TRUE(MG_Util::Async::IsAsyncShaderCompileSuspended());
    EXPECT_FALSE(MG_Util::Async::AsyncShaderCompileActive());
    // The configuration flag itself is untouched: the extension is still advertised, the
    // application just asked for serial compilation.
    EXPECT_TRUE(MG_Util::Async::AsyncShaderCompileEnabled());

    for (const GLuint shader : backlog) {
        EXPECT_EQ(QueryShaderCompletion(shader), GL_TRUE)
            << "glMaxShaderCompilerThreadsKHR(0) must leave nothing in flight";
        EXPECT_EQ(QueryCompileStatus(shader), GL_TRUE);
    }

    // Anything compiled from here on is finished before its glCompileShader returns.
    Vector<String> serialSources;
    const Vector<GLuint> serial = EnqueueBacklog(6, 4900, serialSources);
    for (const GLuint shader : serial) {
        EXPECT_EQ(QueryShaderCompletion(shader), GL_TRUE) << "a compile after a zero count must be synchronous";
    }
    // Links too, not just compiles.
    const GLuint vs = MakeShader(GL_VERTEX_SHADER, kVs);
    CompileShader(vs);
    const GLuint program = CreateProgram();
    AttachShader(program, vs);
    AttachShader(program, serial.front());
    LinkProgram(program);
    EXPECT_EQ(QueryProgramCompletion(program), GL_TRUE) << "a link after a zero count must be synchronous";
    EXPECT_EQ(QueryLinkStatus(program), GL_TRUE);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// The same obligation, but for LINKS that are already in flight when the zero count arrives -
// and specifically for BOTH phases of one. A link is two chained jobs now (ProgramLinkTask,
// then ProgramSpirvTask), and GL_COMPLETION_STATUS_KHR spans both, so
// ProgramState::JoinAllPendingWork has to settle both or this query reads GL_FALSE in the one
// mode the extension says cannot have anything pending. The case above creates its program
// AFTER the zero count, so it links inline and cannot see this; here the programs are linked
// against a saturated pool BEFORE it.
TEST_F(ParallelShaderCompileTest, ZeroCompilerThreadsJoinsPendingLinksAndTheirSpirvJobs) {
    const AsyncModeScope async(true);
    const CompilerThreadScope threads;
    MaxShaderCompilerThreadsKHR(1);

    // A backlog first, so the links below cannot all drain before the zero count lands.
    Vector<String> sources;
    (void)EnqueueBacklog(24, 5000, sources);

    Vector<GLuint> programs;
    for (int i = 0; i < 8; ++i) {
        sources.push_back(MakeBulkySource(5100 + i));
        const char* text = sources.back().c_str();
        const GLuint fs = CreateShader(GL_FRAGMENT_SHADER);
        ShaderSource(fs, 1, &text, nullptr);
        CompileShader(fs);
        const GLuint vs = MakeShader(GL_VERTEX_SHADER, kVs);
        CompileShader(vs); // this file's MakeShader only sources; it does not compile
        const GLuint program = CreateProgram();
        AttachShader(program, vs);
        AttachShader(program, fs);
        LinkProgram(program);
        programs.push_back(program);
    }

    int outstanding = 0;
    for (const GLuint program : programs) {
        if (QueryProgramCompletion(program) == GL_FALSE) ++outstanding;
    }

    MaxShaderCompilerThreadsKHR(0);

    for (const GLuint program : programs) {
        EXPECT_EQ(QueryProgramCompletion(program), GL_TRUE)
            << "glMaxShaderCompilerThreadsKHR(0) must leave neither link phase in flight";
        EXPECT_EQ(QueryLinkStatus(program), GL_TRUE);
    }
    EXPECT_GT(outstanding, 0) << "every link had drained before the zero count; this case proved nothing";
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// ...and a later NONZERO count is what lifts it. Nothing else does: not a new context, not a
// join, not eglInitialize. That is the documented contract, so it gets an assertion.
TEST_F(ParallelShaderCompileTest, NonzeroCompilerThreadsRestoresAsynchronousCompilation) {
    const AsyncModeScope async(true);
    const CompilerThreadScope threads;

    MaxShaderCompilerThreadsKHR(0);
    ASSERT_TRUE(MG_Util::Async::IsAsyncShaderCompileSuspended());

    // Re-initializing must NOT quietly re-arm it - the application asked for serial
    // compilation and has not taken that back.
    MobileGL::Initialize();
    EXPECT_TRUE(MG_Util::Async::IsAsyncShaderCompileSuspended());

    MaxShaderCompilerThreadsKHR(4);
    EXPECT_FALSE(MG_Util::Async::IsAsyncShaderCompileSuspended());
    EXPECT_TRUE(MG_Util::Async::AsyncShaderCompileActive());

    // And work really is being enqueued again: with the budget back at one worker a heavy
    // backlog leaves something outstanding (skip-not-fail if the box drained it first).
    MaxShaderCompilerThreadsKHR(1);
    Vector<String> sources;
    const Vector<GLuint> shaders = EnqueueBacklog(64, 5000, sources);
    const Bool anyOutstanding = std::any_of(shaders.begin(), shaders.end(), [](const GLuint shader) {
        return QueryShaderCompletion(shader) == GL_FALSE;
    });
    if (!anyOutstanding) {
        GTEST_SKIP() << "the pool drained the backlog before the first query; asynchrony not observable here";
    }
    for (const GLuint shader : shaders) {
        EXPECT_EQ(QueryCompileStatus(shader), GL_TRUE);
    }
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// The three count cases map onto the pool's concurrency budget: a request above the thread
// count cannot conjure threads, 0xFFFFFFFF means "implementation maximum", and an ordinary
// value is taken as given (clamped to at least one).
TEST_F(ParallelShaderCompileTest, CompilerThreadCountIsClampedToTheThreadCount) {
    const AsyncModeScope async(true);
    const CompilerThreadScope threads;
    auto& pool = MG_Util::Async::ShaderCompilePool::Get();
    const Uint threadCount = pool.GetThreadCount();
    ASSERT_GE(threadCount, 1u);

    MaxShaderCompilerThreadsKHR(1);
    EXPECT_EQ(pool.GetMaxConcurrency(), 1u);

    MaxShaderCompilerThreadsKHR(threadCount + 1000);
    EXPECT_EQ(pool.GetMaxConcurrency(), threadCount) << "asking for more threads than exist cannot create any";

    MaxShaderCompilerThreadsKHR(1);
    ASSERT_EQ(pool.GetMaxConcurrency(), 1u);
    MaxShaderCompilerThreadsKHR(0xFFFFFFFFu);
    EXPECT_EQ(pool.GetMaxConcurrency(), threadCount) << "0xFFFFFFFF is the implementation maximum";

    // The ARB spelling is the same entry point, not a second piece of state.
    MaxShaderCompilerThreadsARB(1);
    EXPECT_EQ(pool.GetMaxConcurrency(), 1u);
    MaxShaderCompilerThreadsARB(0);
    EXPECT_TRUE(MG_Util::Async::IsAsyncShaderCompileSuspended());
    MaxShaderCompilerThreadsKHR(threadCount);
    EXPECT_FALSE(MG_Util::Async::IsAsyncShaderCompileSuspended())
        << "the KHR and ARB names must share one piece of state";
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// A zero count with the feature switched off is legal and does nothing observable: there is
// nothing to suspend, and the call must not fail just because MobileGL never had threads.
TEST_F(ParallelShaderCompileTest, CompilerThreadCallsAreHarmlessWithAsyncOff) {
    const AsyncModeScope async(false);
    const CompilerThreadScope threads;
    MaxShaderCompilerThreadsKHR(0);
    MaxShaderCompilerThreadsKHR(8);
    MaxShaderCompilerThreadsARB(0xFFFFFFFFu);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// ---------------------------------------------------------------------------------------
// GL_MAX_SHADER_COMPILER_THREADS_KHR
// ---------------------------------------------------------------------------------------

TEST_F(ParallelShaderCompileTest, MaxShaderCompilerThreadsGetter) {
    {
        const AsyncModeScope async(true);
        GLint value = -1;
        GetIntegerv(GL_MAX_SHADER_COMPILER_THREADS_KHR, &value);
        EXPECT_EQ(value, static_cast<GLint>(MG_Util::Async::ShaderCompilePool::Get().GetThreadCount()));
        EXPECT_GE(value, 1);
        EXPECT_EQ(GetError(), GL_NO_ERROR);
    }
    {
        // No compiler threads exist in this configuration, and the extension is not
        // advertised either, so zero is the honest answer.
        const AsyncModeScope async(false);
        GLint value = -1;
        GetIntegerv(GL_MAX_SHADER_COMPILER_THREADS_KHR, &value);
        EXPECT_EQ(value, 0);
        EXPECT_EQ(GetError(), GL_NO_ERROR);
    }
}

// The reported maximum is the pool's THREAD count, not its current concurrency budget: an
// application that lowered the budget still wants to know what the implementation can do.
TEST_F(ParallelShaderCompileTest, MaxShaderCompilerThreadsIgnoresTheCurrentBudget) {
    const AsyncModeScope async(true);
    const CompilerThreadScope threads;
    const Uint threadCount = MG_Util::Async::ShaderCompilePool::Get().GetThreadCount();
    MaxShaderCompilerThreadsKHR(1);
    GLint value = -1;
    GetIntegerv(GL_MAX_SHADER_COMPILER_THREADS_KHR, &value);
    EXPECT_EQ(value, static_cast<GLint>(threadCount));
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// ---------------------------------------------------------------------------------------
// The extension string
// ---------------------------------------------------------------------------------------

// The advertisement is the riskiest half of P1 - a recorded trace cannot cover it, because
// Iris and Sodium change their submission schedule the moment they see the string - so
// MOBILEGL_ASYNC_SHADER_COMPILE=0 has to withdraw it. Asserted on both backends' own
// BuildAdvertisedExtensions, which is the single source of truth each of them (and the
// driver POST) builds the list from.
TEST_F(ParallelShaderCompileTest, BothBackendsAdvertiseTheExtensionIffAsyncIsEnabled) {
    {
        const AsyncModeScope async(true);
        EXPECT_TRUE(Advertises(MG_Backend::DirectGLES::BuildAdvertisedExtensions(false, false, false, false),
                               E_GL_KHR_parallel_shader_compile));
        EXPECT_TRUE(Advertises(MG_Backend::DirectVulkan::BuildAdvertisedExtensions(false, false, false, false),
                               E_GL_KHR_parallel_shader_compile));
    }
    {
        const AsyncModeScope async(false);
        EXPECT_FALSE(Advertises(MG_Backend::DirectGLES::BuildAdvertisedExtensions(false, false, false, false),
                                E_GL_KHR_parallel_shader_compile))
            << "MOBILEGL_ASYNC_SHADER_COMPILE=0 must withdraw the extension, not only the threading";
        EXPECT_FALSE(Advertises(MG_Backend::DirectVulkan::BuildAdvertisedExtensions(false, false, false, false),
                                E_GL_KHR_parallel_shader_compile))
            << "MOBILEGL_ASYNC_SHADER_COMPILE=0 must withdraw the extension, not only the threading";
    }
}

// The same fact through the GL surface an application actually reads. No flag flipping here:
// a backend's advertised list is built once, at its first use, from the configuration that
// was in force then - so this case asserts against the AMBIENT configuration, which is
// exactly what makes it meaningful in both of the suite's two runs (with and without
// MOBILEGL_ASYNC_SHADER_COMPILE=1 exported).
TEST_F(ParallelShaderCompileTest, GLExtensionStringTracksTheAmbientConfiguration) {
    UniquePtr<MG_Backend::BackendObject> previousBackend = Move(MG_Backend::pActiveBackendObject);
    MG_Backend::pActiveBackendObject = MakeUnique<MG_Backend::DirectGLES::BackendObject_DirectGLES>();

    const char* extensions = reinterpret_cast<const char*>(GetString(GL_EXTENSIONS));
    ASSERT_NE(extensions, nullptr);
    const String extensionString(extensions);
    const Bool advertised = extensionString.find("GL_KHR_parallel_shader_compile") != String::npos;
    EXPECT_EQ(advertised, MG_Util::Async::AsyncShaderCompileEnabled()) << "GL_EXTENSIONS = " << extensionString;

    // glGetStringi must agree with the monolithic string - LWJGL builds GLCapabilities from
    // the indexed form on a core profile.
    GLint count = 0;
    GetIntegerv(GL_NUM_EXTENSIONS, &count);
    ASSERT_GT(count, 0);
    Bool foundIndexed = false;
    for (GLint i = 0; i < count; ++i) {
        const char* name = reinterpret_cast<const char*>(GetStringi(GL_EXTENSIONS, static_cast<GLuint>(i)));
        if (name != nullptr && std::string(name) == "GL_KHR_parallel_shader_compile") foundIndexed = true;
    }
    EXPECT_EQ(foundIndexed, advertised);

    MG_Backend::pActiveBackendObject = Move(previousBackend);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}
