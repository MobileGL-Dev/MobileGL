// MobileGL - MobileGL/MG_Test/Program/ShaderCompileAdoptionTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// P1 stage 6: two shader objects handed byte-identical source share ONE compile job.
//
// The property under test is a conjunction, and every case here attacks one half of it:
//   * the sharing itself - one job, one node, both GL names reporting the same answer, and
//     two programs linking that one node to byte-identical SPIR-V;
//   * that sharing did not make a cancel dangerous. Before this stage a node had exactly one
//     shader object, so "this object stopped caring" and "nothing can observe this result"
//     were the same statement and CancelCompile() cancelled unconditionally. They are not the
//     same statement any more, and the four mutation paths that used to reach that cancel -
//     re-source, delete, the orphan-name sweep, the destructor - are each covered below with
//     a second object still holding the node.
//
// Like the other async suites, every case flips MG_Config::Features.AsyncShaderCompile itself
// and drives the real GL entry points, so the file behaves identically whether or not the
// suite was launched with MOBILEGL_ASYNC_SHADER_COMPILE=1.
//
// Adoption is decided ON THE GL THREAD, before anything is posted, so the counter assertions
// here are deterministic rather than timing-dependent: whether the first object's compile has
// already finished changes nothing about whether the second one adopts it.

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "Config.h"
#include "Includes.h"
#include "Init.h"
#include "MG_Impl/GLImpl/Getter/GL_Getter.h"
#include "MG_Impl/GLImpl/Program/GL_Program.h"
#include "MG_State/GLState/Core.h"
#include "MG_State/GLState/ProgramState/ShaderCompileAdoptionMap.h"
#include "MG_State/GLState/ProgramState/ShaderCompileTask.h"
#include "MG_State/GLState/ProgramState/ShaderPreprocessCache.h"
#include "MG_Util/Async/ShaderCompilePool.h"
#include "MG_Util/ShaderTranspiler/CompileEnv.h"

using namespace MobileGL;
using namespace MobileGL::MG_Impl::GLImpl;
using MobileGL::MG_State::GLState::ShaderCompileAdoptionMap;
using MobileGL::MG_State::GLState::ShaderCompileTask;
using MobileGL::MG_State::GLState::ShaderObject;
using MobileGL::MG_State::GLState::ShaderPreprocessCache;

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

    // The suspension latch and the concurrency budget are PROCESS-wide, so a case that
    // touches either has to put both back or it poisons every case after it in this binary.
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
uniform mat4 uModel;
uniform vec4 uColor;
out vec4 vColor;
void main() {
    vColor = uColor;
    gl_Position = uModel * vec4(aPos, 1.0);
}
)";

    // Fails inside glslang rather than in the lexical pre-checks, so it exercises the same
    // ParseFailed path a real broken shaderpack source takes.
    const char* kBrokenFs = R"(#version 460
layout(location = 0) out vec4 fragColor;
void main() { fragColor = thisIdentifierWasNeverDeclared; }
)";

    // Big enough that a compile is not instantaneous, so a duplicate really would cost
    // something. Templated on an index so every instance is a distinct source.
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

    // Heavy enough that a spinning GL thread can reliably observe the compile Running on a
    // single-worker pool, for RunningCancelRequestedNodeIsNotAdopted below - MakeBulkySource
    // is tuned for "not instantaneous", this one is tuned for "actually spin-observable".
    String MakeVeryHeavySource(const int index) {
        String source = "#version 460\nlayout(location = 0) out vec4 fragColor;\n";
        source += "uniform float uSeed" + std::to_string(index) + ";\n";
        source += "void main() {\n    float acc = uSeed" + std::to_string(index) + ";\n";
        for (int i = 0; i < 4000; ++i) {
            source += "    acc = acc * 1.0001 + sin(acc + " + std::to_string(i) + ".0) * cos(acc);\n";
        }
        source += "    fragColor = vec4(acc, acc, acc, 1.0);\n}\n";
        return source;
    }

    Uint64 AdoptionCount() {
        return MG_State::pGLContext->GetShaderCompileAdoptionMap().GetAdoptionCount();
    }

    // A copy of the slot, never the reference: creating another shader can reallocate the
    // context's object table.
    SharedPtr<ShaderObject> Object(const GLuint shader) {
        return MG_State::pGLContext->GetShaderObject(shader);
    }

    // The node identity, WITHOUT joining - this is what "they share one job" means, and
    // asking must not settle anything.
    const ShaderCompileTask* NodeOf(const GLuint shader) {
        const SharedPtr<ShaderObject> object = Object(shader);
        return object ? object->CompiledNodeForLink().get() : nullptr;
    }

    GLuint MakeShader(const GLenum type, const char* source) {
        const GLuint shader = CreateShader(type);
        ShaderSource(shader, 1, &source, nullptr);
        return shader;
    }

    GLuint MakeAndCompile(const GLenum type, const char* source) {
        const GLuint shader = MakeShader(type, source);
        CompileShader(shader);
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

    // Content hash of a linked program's generated SPIR-V, through the state layer (there is
    // no GL query for it). This is what catches a mis-shared parse: if the claim CAS on a
    // SHARED node let two links both run mapIO over the same intermediate, the two programs
    // would disagree here.
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

    // Enqueues `count` distinct heavy compiles and reads nothing back, so the pool is left
    // with a real backlog for the caller's mutations to race against.
    void SaturatePool(const int count, Vector<String>& sourceStorage) {
        sourceStorage.reserve(sourceStorage.size() + static_cast<SizeT>(count));
        for (int i = 0; i < count; ++i) {
            sourceStorage.push_back(MakeBulkySource(90000 + i));
            const char* text = sourceStorage.back().c_str();
            const GLuint shader = CreateShader(GL_FRAGMENT_SHADER);
            ShaderSource(shader, 1, &text, nullptr);
            CompileShader(shader);
        }
    }

    // Links `shader` against a freshly compiled vertex stage and returns the program.
    GLuint LinkWith(const GLuint shader) {
        const GLuint vs = MakeAndCompile(GL_VERTEX_SHADER, kVs);
        const GLuint program = CreateProgram();
        AttachShader(program, vs);
        AttachShader(program, shader);
        LinkProgram(program);
        return program;
    }

    class ShaderCompileAdoptionTest : public ::testing::Test {
    protected:
        void SetUp() override { MobileGL::Initialize(); }
    };
} // namespace

// ---------------------------------------------------------------------------------------
// The sharing itself
// ---------------------------------------------------------------------------------------

// The headline: two GL shader names, byte-identical source, exactly one job. Both names must
// answer every query correctly, and the ONE parse they share must link into two separate
// programs with byte-identical SPIR-V - which is the stage-4 claim CAS being exercised on a
// shared node for the first time.
TEST_F(ShaderCompileAdoptionTest, TwoObjectsWithIdenticalSourceShareOneCompileJob) {
    const AsyncModeScope async(true);
    Vector<String> backlog;
    SaturatePool(32, backlog);

    const String source = MakeBulkySource(100);
    const char* text = source.c_str();

    const Uint64 before = AdoptionCount();
    const GLuint a = CreateShader(GL_FRAGMENT_SHADER);
    ShaderSource(a, 1, &text, nullptr);
    CompileShader(a);
    const GLuint b = CreateShader(GL_FRAGMENT_SHADER);
    ShaderSource(b, 1, &text, nullptr);
    CompileShader(b);

    EXPECT_EQ(AdoptionCount() - before, 1u) << "the second glCompileShader must not enqueue a duplicate";
    ASSERT_NE(NodeOf(a), nullptr);
    EXPECT_EQ(NodeOf(a), NodeOf(b)) << "both objects must hold the very same job node";

    // Both names still answer for themselves.
    EXPECT_EQ(QueryCompileStatus(a), GL_TRUE) << QueryShaderInfoLog(a);
    EXPECT_EQ(QueryCompileStatus(b), GL_TRUE) << QueryShaderInfoLog(b);
    EXPECT_EQ(QueryShaderInfoLog(a), QueryShaderInfoLog(b));
    EXPECT_TRUE(QueryShaderInfoLog(a).empty());

    // One node, two links: exactly one of them wins ClaimParsedShader, the other re-parses,
    // and the two must agree bit for bit.
    const GLuint programA = LinkWith(a);
    const GLuint programB = LinkWith(b);
    ASSERT_EQ(QueryLinkStatus(programA), GL_TRUE);
    ASSERT_EQ(QueryLinkStatus(programB), GL_TRUE);
    const Vector<Uint64> digestA = SpirvDigest(programA);
    const Vector<Uint64> digestB = SpirvDigest(programB);
    ASSERT_EQ(digestA.size(), 2u);
    EXPECT_EQ(digestA, digestB) << "a shared node linked twice produced different SPIR-V";
    EXPECT_GE(GetUniformLocation(programA, "uSeed100"), 0);
    EXPECT_GE(GetUniformLocation(programB, "uSeed100"), 0);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// Adoption must also re-arm the adopter's layer-1 memo. It is a POINTER comparison against
// the node's own source snapshot, so an adopter that kept its own equal-but-distinct copy
// would decide on the very next glCompileShader that it had no memo and enqueue the exact
// duplicate this stage exists to remove - and an identical glShaderSource would cancel a
// compile another object is still waiting on.
TEST_F(ShaderCompileAdoptionTest, AdoptingAlsoArmsTheLayerOneMemo) {
    const AsyncModeScope async(true);
    Vector<String> backlog;
    SaturatePool(32, backlog);

    const String source = MakeBulkySource(110);
    const char* text = source.c_str();

    const GLuint a = CreateShader(GL_FRAGMENT_SHADER);
    ShaderSource(a, 1, &text, nullptr);
    CompileShader(a);
    const GLuint b = CreateShader(GL_FRAGMENT_SHADER);
    ShaderSource(b, 1, &text, nullptr);
    CompileShader(b);

    const SharedPtr<ShaderObject> objectB = Object(b);
    ASSERT_NE(objectB, nullptr);
    EXPECT_TRUE(objectB->HasMemoizedCompile()) << "an adopted node must satisfy the layer-1 memo";

    const ShaderCompileTask* shared = NodeOf(b);
    const Uint64 before = AdoptionCount();
    for (int i = 0; i < 4; ++i) {
        CompileShader(b);
        EXPECT_EQ(NodeOf(b), shared) << "a repeat glCompileShader on an adopter must be a no-op";
    }
    // A byte-identical re-source is a no-op too, so it must not disturb the shared node.
    ShaderSource(b, 1, &text, nullptr);
    EXPECT_EQ(NodeOf(b), shared);
    EXPECT_EQ(AdoptionCount(), before) << "no-op calls must not even reach the adoption map";

    EXPECT_EQ(QueryCompileStatus(a), GL_TRUE) << QueryShaderInfoLog(a);
    EXPECT_EQ(QueryCompileStatus(b), GL_TRUE) << QueryShaderInfoLog(b);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// Different source, and same source in a different STAGE, are different keys. This is the
// guard against the map ever handing out a node that does not belong to the caller.
TEST_F(ShaderCompileAdoptionTest, DifferentSourceOrStageIsNotAdopted) {
    const AsyncModeScope async(true);

    const String first = MakeBulkySource(120);
    const String second = MakeBulkySource(121);
    const char* firstText = first.c_str();
    const char* secondText = second.c_str();

    const Uint64 before = AdoptionCount();
    const GLuint a = CreateShader(GL_FRAGMENT_SHADER);
    ShaderSource(a, 1, &firstText, nullptr);
    CompileShader(a);
    const GLuint b = CreateShader(GL_FRAGMENT_SHADER);
    ShaderSource(b, 1, &secondText, nullptr);
    CompileShader(b);
    EXPECT_EQ(AdoptionCount(), before) << "different text must not adopt";
    EXPECT_NE(NodeOf(a), NodeOf(b));

    // The same text in two stages: the vertex/fragment pair below shares no node either,
    // because the stage is part of the key.
    const GLuint vsA = MakeAndCompile(GL_VERTEX_SHADER, kVs);
    const GLuint vsB = MakeAndCompile(GL_VERTEX_SHADER, kVs);
    EXPECT_EQ(NodeOf(vsA), NodeOf(vsB)) << "same stage, same text: must share";
    EXPECT_NE(NodeOf(vsA), NodeOf(a));

    EXPECT_EQ(QueryCompileStatus(a), GL_TRUE) << QueryShaderInfoLog(a);
    EXPECT_EQ(QueryCompileStatus(b), GL_TRUE) << QueryShaderInfoLog(b);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// A failed compile is shared exactly like a successful one, and both names must report the
// identical status and the identical log - the info log lives in the node's artifacts, so
// this is also the guard that a second joiner is not left with an empty one.
TEST_F(ShaderCompileAdoptionTest, AdoptedFailingCompileReportsTheIdenticalLogToBothObjects) {
    const AsyncModeScope async(true);
    Vector<String> backlog;
    SaturatePool(32, backlog);

    const Uint64 before = AdoptionCount();
    const GLuint a = MakeAndCompile(GL_FRAGMENT_SHADER, kBrokenFs);
    const GLuint b = MakeAndCompile(GL_FRAGMENT_SHADER, kBrokenFs);
    EXPECT_EQ(AdoptionCount() - before, 1u);
    EXPECT_EQ(NodeOf(a), NodeOf(b));

    EXPECT_EQ(QueryCompileStatus(a), GL_FALSE);
    EXPECT_EQ(QueryCompileStatus(b), GL_FALSE);
    const String logA = QueryShaderInfoLog(a);
    EXPECT_FALSE(logA.empty());
    EXPECT_EQ(QueryShaderInfoLog(b), logA);
    // GL models a failed compile as status + log, never as a GL error - which is what makes
    // moving the work off-thread (and sharing it) legal at all.
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// ---------------------------------------------------------------------------------------
// The four release paths, each with a second object still holding the node
// ---------------------------------------------------------------------------------------

// glShaderSource with DIFFERENT text on one sharer. Its release must NOT cancel the node the
// other one is still waiting on; the re-sourced object gets a fresh compile of its own.
TEST_F(ShaderCompileAdoptionTest, ResourcingOneSharerLeavesTheOtherIntact) {
    const AsyncModeScope async(true);
    Vector<String> backlog;
    SaturatePool(48, backlog);

    const String shared = MakeBulkySource(200);
    const char* sharedText = shared.c_str();
    const GLuint a = CreateShader(GL_FRAGMENT_SHADER);
    ShaderSource(a, 1, &sharedText, nullptr);
    CompileShader(a);
    const GLuint b = CreateShader(GL_FRAGMENT_SHADER);
    ShaderSource(b, 1, &sharedText, nullptr);
    CompileShader(b);
    const ShaderCompileTask* sharedNode = NodeOf(b);
    ASSERT_NE(sharedNode, nullptr);
    ASSERT_EQ(NodeOf(a), sharedNode);

    // Replace A's text while the shared compile is very probably still outstanding.
    const String replacement = MakeBulkySource(201);
    const char* replacementText = replacement.c_str();
    ShaderSource(a, 1, &replacementText, nullptr);
    EXPECT_EQ(NodeOf(a), nullptr) << "a real source change must drop the object's node";
    EXPECT_EQ(NodeOf(b), sharedNode) << "B must still hold the shared node";

    // B is untouched: the compile it is waiting on still publishes, and its artifacts are
    // the ones that source really produces.
    ASSERT_EQ(QueryCompileStatus(b), GL_TRUE) << QueryShaderInfoLog(b);
    const GLuint programB = LinkWith(b);
    ASSERT_EQ(QueryLinkStatus(programB), GL_TRUE);
    EXPECT_GE(GetUniformLocation(programB, "uSeed200"), 0);

    // A gets a genuinely fresh compile of the new text.
    CompileShader(a);
    EXPECT_NE(NodeOf(a), sharedNode);
    ASSERT_EQ(QueryCompileStatus(a), GL_TRUE) << QueryShaderInfoLog(a);
    const GLuint programA = LinkWith(a);
    ASSERT_EQ(QueryLinkStatus(programA), GL_TRUE);
    EXPECT_GE(GetUniformLocation(programA, "uSeed201"), 0);
    EXPECT_EQ(GetUniformLocation(programA, "uSeed200"), -1);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// glDeleteShader on one sharer. The name goes immediately (no wait for a worker) and the
// object is destroyed, so this covers the DESTRUCTOR release as well as the orphan sweep's.
TEST_F(ShaderCompileAdoptionTest, DeletingOneSharerLeavesTheOtherIntact) {
    const AsyncModeScope async(true);
    Vector<String> backlog;
    SaturatePool(48, backlog);

    const String source = MakeBulkySource(210);
    const char* text = source.c_str();
    const GLuint a = CreateShader(GL_FRAGMENT_SHADER);
    ShaderSource(a, 1, &text, nullptr);
    CompileShader(a);
    const GLuint b = CreateShader(GL_FRAGMENT_SHADER);
    ShaderSource(b, 1, &text, nullptr);
    CompileShader(b);
    const ShaderCompileTask* sharedNode = NodeOf(b);
    ASSERT_NE(sharedNode, nullptr);
    ASSERT_EQ(NodeOf(a), sharedNode);

    DeleteShader(a);
    EXPECT_EQ(IsShader(a), GL_FALSE) << "an unattached deleted shader's name goes immediately";
    EXPECT_EQ(NodeOf(b), sharedNode);

    ASSERT_EQ(QueryCompileStatus(b), GL_TRUE) << QueryShaderInfoLog(b);
    const GLuint program = LinkWith(b);
    ASSERT_EQ(QueryLinkStatus(program), GL_TRUE);
    EXPECT_GE(GetUniformLocation(program, "uSeed210"), 0);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// The window DeletingOneSharerLeavesTheOtherIntact cannot reach: there, A's compile has
// always already finished (or not yet started) by the time B adopts, because the pool is
// merely BUSY with other backlog. Here A's OWN node is still Running - a worker is inside
// RunBody() for it - when the last holder releases it. ReleaseCompileNode fires Cancel(),
// but JobNode::Cancel on a Running node only sets the cancellation-REQUEST flag; the state
// stays Running until the worker's body returns and JobNode::Run forces the final transition
// to Cancelled (see JobNode::Run's tail: it takes Cancelled instead of Complete whenever
// m_cancelled is set, regardless of how the body finished). FindAdoptable must refuse a node
// in that in-between state - not just one already settled as Cancelled - or C inherits a
// doomed node and glGetShaderiv reports GL_FALSE with an empty info log for valid source.
TEST_F(ShaderCompileAdoptionTest, RunningCancelRequestedNodeIsNotAdopted) {
    const AsyncModeScope async(true);
    MG_Util::Async::ShaderCompilePool::Get().SetMaxConcurrency(1);

    const String source = MakeVeryHeavySource(310);
    const char* text = source.c_str();
    const GLuint a = CreateShader(GL_FRAGMENT_SHADER);
    ShaderSource(a, 1, &text, nullptr);
    CompileShader(a);

    // Spin on the GL thread until the single worker is actually inside A's body. The source
    // is sized to make that window observable rather than instantaneous.
    const ShaderCompileTask* node = NodeOf(a);
    ASSERT_NE(node, nullptr);
    bool sawRunning = false;
    for (int i = 0; i < 200000 && !node->IsTerminal(); ++i) {
        if (node->State() == MG_Util::Async::JobState::Running) {
            sawRunning = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(20));
    }
    ASSERT_TRUE(sawRunning) << "could not observe A's compile Running; the synthetic source "
                               "needs to be heavier, or the pool did not have a free worker";

    // A is the ONLY holder, so this release brings the adopter count to zero and (with no
    // link pin) fires Cancel() on a node that is still Running.
    DeleteShader(a);
    ASSERT_EQ(node->State(), MG_Util::Async::JobState::Running)
        << "the node already settled; the race window closed before the assertions below "
           "could observe it - widen MakeVeryHeavySource's loop count";
    ASSERT_TRUE(node->IsCancellationRequested());
    ASSERT_FALSE(node->IsCancelled()) << "the window this test targets does not exist here";

    // A brand-new shader name, byte-identical source, nothing wrong with it.
    const GLuint c = CreateShader(GL_FRAGMENT_SHADER);
    ShaderSource(c, 1, &text, nullptr);
    CompileShader(c);
    EXPECT_NE(NodeOf(c), node) << "C adopted a cancellation-requested, still-Running node";

    ASSERT_EQ(QueryCompileStatus(c), GL_TRUE)
        << "valid source reported GL_FALSE; info log: [" << QueryShaderInfoLog(c) << "]";
    EXPECT_EQ(GetError(), GL_NO_ERROR);

    MG_Util::Async::ShaderCompilePool::Get().SetMaxConcurrency(
        MG_Util::Async::ShaderCompilePool::Get().GetThreadCount());
}

// The deferred half of glDeleteShader: A is ATTACHED, so the delete only flags it and the
// name is freed by ReleaseShaderNameIfOrphaned when the detach removes the last GL-visible
// attachment. That sweep is the other caller of the release path, and it must not cancel the
// node B is sharing.
TEST_F(ShaderCompileAdoptionTest, OrphanSweepOnOneSharerLeavesTheOtherIntact) {
    const AsyncModeScope async(true);
    Vector<String> backlog;
    SaturatePool(48, backlog);

    const String source = MakeBulkySource(220);
    const char* text = source.c_str();
    const GLuint a = CreateShader(GL_FRAGMENT_SHADER);
    ShaderSource(a, 1, &text, nullptr);
    CompileShader(a);
    const GLuint b = CreateShader(GL_FRAGMENT_SHADER);
    ShaderSource(b, 1, &text, nullptr);
    CompileShader(b);
    const ShaderCompileTask* sharedNode = NodeOf(b);
    ASSERT_NE(sharedNode, nullptr);
    ASSERT_EQ(NodeOf(a), sharedNode);

    // Attach A, flag it for deletion (name survives), then detach: the sweep fires here, with
    // NO link ever posted, so the stage-4 pin is NOT what is protecting the node - only the
    // adopter count is.
    const GLuint program = CreateProgram();
    AttachShader(program, a);
    DeleteShader(a);
    EXPECT_EQ(IsShader(a), GL_TRUE) << "an attached deleted shader keeps its name";
    DetachShader(program, a);
    EXPECT_EQ(IsShader(a), GL_FALSE) << "the detach must free the flagged shader's name";
    EXPECT_EQ(NodeOf(b), sharedNode);

    ASSERT_EQ(QueryCompileStatus(b), GL_TRUE) << QueryShaderInfoLog(b);
    const GLuint programB = LinkWith(b);
    ASSERT_EQ(QueryLinkStatus(programB), GL_TRUE);
    EXPECT_GE(GetUniformLocation(programB, "uSeed220"), 0);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// The same sweep, now with the stage-4 link pin also in play: A's program is LINKED (so the
// node is MarkLinkReferenced) and then A is detached and deleted, while B still shares the
// node. Both protections have to hold at once - the link must report GL_TRUE and B must
// still compile.
TEST_F(ShaderCompileAdoptionTest, OrphanSweepWithALinkPinnedSharedNodeHoldsBoth) {
    const AsyncModeScope async(true);
    Vector<String> backlog;
    SaturatePool(48, backlog);

    const String source = MakeBulkySource(230);
    const char* text = source.c_str();
    const GLuint a = CreateShader(GL_FRAGMENT_SHADER);
    ShaderSource(a, 1, &text, nullptr);
    CompileShader(a);
    const GLuint b = CreateShader(GL_FRAGMENT_SHADER);
    ShaderSource(b, 1, &text, nullptr);
    CompileShader(b);
    const ShaderCompileTask* sharedNode = NodeOf(b);
    ASSERT_NE(sharedNode, nullptr);
    ASSERT_EQ(NodeOf(a), sharedNode);

    // The ordinary teardown order: link, then detach, then delete. No status read in between,
    // so the link's own prologue is what joins the shared compile.
    const GLuint vs = MakeAndCompile(GL_VERTEX_SHADER, kVs);
    const GLuint program = CreateProgram();
    AttachShader(program, vs);
    AttachShader(program, a);
    LinkProgram(program);
    DetachShader(program, a);
    DeleteShader(a);
    EXPECT_EQ(IsShader(a), GL_FALSE);

    ASSERT_EQ(QueryLinkStatus(program), GL_TRUE) << "the pinned shared compile must still publish";
    EXPECT_GE(GetUniformLocation(program, "uSeed230"), 0);
    EXPECT_EQ(NodeOf(b), sharedNode);
    ASSERT_EQ(QueryCompileStatus(b), GL_TRUE) << QueryShaderInfoLog(b);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// Every sharer released, in turn, with nothing pinning the node: the LAST release is the one
// that may cancel, and afterwards the map must not hand the cancelled node to anybody. The
// property asserted is the one that matters and it is timing-free: whatever happened to the
// old node, a later object with the same source must end up with a CORRECT compile.
TEST_F(ShaderCompileAdoptionTest, AfterEverySharerIsGoneTheNextCompileIsStillCorrect) {
    const AsyncModeScope async(true);
    const CompilerThreadScope compilerThreads;
    // One worker and a deep backlog: a node posted now is overwhelmingly likely to still be
    // queued when its last holder drops it, which is the state in which the cancel bites.
    MG_Util::Async::ShaderCompilePool::Get().SetMaxConcurrency(1);
    Vector<String> backlog;
    SaturatePool(48, backlog);

    const String source = MakeBulkySource(240);
    const char* text = source.c_str();
    const GLuint a = CreateShader(GL_FRAGMENT_SHADER);
    ShaderSource(a, 1, &text, nullptr);
    CompileShader(a);
    const GLuint b = CreateShader(GL_FRAGMENT_SHADER);
    ShaderSource(b, 1, &text, nullptr);
    CompileShader(b);
    ASSERT_EQ(NodeOf(a), NodeOf(b));

    DeleteShader(a);
    DeleteShader(b); // the last holder: this one is authorized to cancel

    const GLuint c = CreateShader(GL_FRAGMENT_SHADER);
    ShaderSource(c, 1, &text, nullptr);
    CompileShader(c);
    ASSERT_EQ(QueryCompileStatus(c), GL_TRUE)
        << "a cancelled node must never be adopted - it can only ever report GL_FALSE. "
        << QueryShaderInfoLog(c);
    const GLuint program = LinkWith(c);
    ASSERT_EQ(QueryLinkStatus(program), GL_TRUE);
    EXPECT_GE(GetUniformLocation(program, "uSeed240"), 0);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// ---------------------------------------------------------------------------------------
// The bypasses: both must be byte-identical to the pre-stage-6 behaviour
// ---------------------------------------------------------------------------------------

// The kill switch. With the flag off, compilation is synchronous and NOTHING is adopted -
// the map is not even consulted, so the counter cannot move.
TEST_F(ShaderCompileAdoptionTest, FlagOffAdoptsNothing) {
    const AsyncModeScope async(false);
    ASSERT_FALSE(MG_Util::Async::AsyncShaderCompileEnabled());

    const String source = MakeBulkySource(300);
    const char* text = source.c_str();

    const Uint64 before = AdoptionCount();
    Vector<GLuint> shaders;
    for (int i = 0; i < 6; ++i) {
        const GLuint fs = CreateShader(GL_FRAGMENT_SHADER);
        ShaderSource(fs, 1, &text, nullptr);
        CompileShader(fs);
        shaders.push_back(fs);
    }
    EXPECT_EQ(AdoptionCount(), before) << "the flag-off path must not consult the adoption map";
    for (SizeT i = 1; i < shaders.size(); ++i) {
        EXPECT_NE(NodeOf(shaders[i]), NodeOf(shaders[0])) << "flag off means one node per object";
    }
    for (const GLuint fs : shaders) {
        EXPECT_EQ(QueryCompileStatus(fs), GL_TRUE) << QueryShaderInfoLog(fs);
    }
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// glMaxShaderCompilerThreadsKHR(0) puts compilation back on the application's thread even
// though the extension stays advertised. Adoption keys off the same predicate, so a
// suspended context shares nothing either - which is what keeps a subsequent
// GL_COMPLETION_STATUS_KHR immediately GL_TRUE without any reasoning about shared nodes.
TEST_F(ShaderCompileAdoptionTest, SuspendedCompilationAdoptsNothing) {
    const AsyncModeScope async(true);
    const CompilerThreadScope compilerThreads;
    MaxShaderCompilerThreadsKHR(0);
    ASSERT_TRUE(MG_Util::Async::IsAsyncShaderCompileSuspended());

    const String source = MakeBulkySource(310);
    const char* text = source.c_str();

    const Uint64 before = AdoptionCount();
    Vector<GLuint> shaders;
    for (int i = 0; i < 4; ++i) {
        const GLuint fs = CreateShader(GL_FRAGMENT_SHADER);
        ShaderSource(fs, 1, &text, nullptr);
        CompileShader(fs);
        shaders.push_back(fs);
        GLint complete = GL_FALSE;
        GetShaderiv(fs, GL_COMPLETION_STATUS_KHR, &complete);
        EXPECT_EQ(complete, GL_TRUE) << "a zero compiler-thread count leaves nothing in flight";
    }
    EXPECT_EQ(AdoptionCount(), before);
    for (SizeT i = 1; i < shaders.size(); ++i) {
        EXPECT_NE(NodeOf(shaders[i]), NodeOf(shaders[0]));
    }
    for (const GLuint fs : shaders) {
        EXPECT_EQ(QueryCompileStatus(fs), GL_TRUE) << QueryShaderInfoLog(fs);
    }
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// ---------------------------------------------------------------------------------------
// Stress
// ---------------------------------------------------------------------------------------

// The shaderpack shape: 48 objects over 6 distinct sources, all enqueued before anything is
// read, on a two-worker pool. 42 of the 48 compiles must simply vanish, and all 48 objects
// must still be individually correct - each with its own name, its own status, and its own
// link (which means 48 claims against 6 shared parses).
TEST_F(ShaderCompileAdoptionTest, StressFortyEightObjectsOverSixSources) {
    const AsyncModeScope async(true);
    const CompilerThreadScope compilerThreads;
    MG_Util::Async::ShaderCompilePool::Get().SetMaxConcurrency(2);

    constexpr int kDistinct = 6;
    constexpr int kDuplicates = 8;
    Vector<String> sources;
    sources.reserve(kDistinct);
    for (int i = 0; i < kDistinct; ++i) {
        sources.push_back(MakeBulkySource(400 + i));
    }

    const Uint64 before = AdoptionCount();
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
    const Uint64 adoptions = AdoptionCount() - before;
    // The floor the stage contracts for, with room for any future scheduling slack...
    ASSERT_GE(adoptions, 30u) << "48 objects over 6 sources adopted only " << adoptions << " times";
    // ...and the number this design actually produces, because the decision is made on the GL
    // thread before anything is posted and therefore does not depend on the workers at all.
    EXPECT_EQ(adoptions, static_cast<Uint64>(kDistinct * (kDuplicates - 1)));

    for (SizeT s = 0; s < shaders.size(); ++s) {
        const GLuint fs = shaders[s];
        ASSERT_EQ(QueryCompileStatus(fs), GL_TRUE) << "shader " << s << ": " << QueryShaderInfoLog(fs);
        const GLuint program = LinkWith(fs);
        ASSERT_EQ(QueryLinkStatus(program), GL_TRUE) << "shader index " << s;
        const String uniform = "uSeed" + std::to_string(400 + static_cast<int>(s % kDistinct));
        EXPECT_GE(GetUniformLocation(program, uniform.c_str()), 0) << uniform;
    }
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// The adversarial interleaving, with duplicates everywhere: compile, query, re-source,
// re-compile, delete, all with the pool busy and most objects sharing nodes. Nothing here
// asserts timing - what it hunts for is a node cancelled out from under a sharer, which
// surfaces as a wrong status, a wrong uniform, or a crash.
TEST_F(ShaderCompileAdoptionTest, StressSharedNodesUnderResourceAndDelete) {
    const AsyncModeScope async(true);
    constexpr int kRounds = 6;
    constexpr int kPerRound = 12;

    for (int round = 0; round < kRounds; ++round) {
        Vector<String> sources;
        sources.reserve(4);
        for (int i = 0; i < 4; ++i) {
            sources.push_back(MakeBulkySource(round * 100 + i));
        }
        const String replacement = MakeBulkySource(round * 100 + 50);
        const char* replacementText = replacement.c_str();

        Vector<GLuint> shaders;
        for (int i = 0; i < kPerRound; ++i) {
            const char* text = sources[static_cast<SizeT>(i % 4)].c_str();
            const GLuint fs = CreateShader(GL_FRAGMENT_SHADER);
            ShaderSource(fs, 1, &text, nullptr);
            CompileShader(fs);
            shaders.push_back(fs);
        }

        // Re-source a third of them onto ONE new shared source, so the survivors of each
        // original node keep waiting on it while the movers pile onto a new one.
        for (int i = 0; i < kPerRound; i += 3) {
            ShaderSource(shaders[static_cast<SizeT>(i)], 1, &replacementText, nullptr);
            CompileShader(shaders[static_cast<SizeT>(i)]);
        }
        // And delete another third outright, while their nodes are still shared.
        for (int i = 1; i < kPerRound; i += 3) {
            DeleteShader(shaders[static_cast<SizeT>(i)]);
        }

        for (int i = 0; i < kPerRound; ++i) {
            if (i % 3 == 1) continue; // deleted
            const GLuint shader = shaders[static_cast<SizeT>(i)];
            ASSERT_EQ(QueryCompileStatus(shader), GL_TRUE)
                << "round " << round << " shader " << i << ": " << QueryShaderInfoLog(shader);
            const GLuint program = LinkWith(shader);
            ASSERT_EQ(QueryLinkStatus(program), GL_TRUE) << "round " << round << " shader " << i;
            const String expected =
                "uSeed" + std::to_string(i % 3 == 0 ? round * 100 + 50 : round * 100 + (i % 4));
            EXPECT_GE(GetUniformLocation(program, expected.c_str()), 0)
                << "round " << round << " shader " << i << " expected " << expected;
            DeleteProgram(program);
        }
        for (int i = 0; i < kPerRound; ++i) {
            if (i % 3 != 1) DeleteShader(shaders[static_cast<SizeT>(i)]);
        }
        EXPECT_EQ(GetError(), GL_NO_ERROR);
    }
}

// ---------------------------------------------------------------------------------------
// The map itself, driven directly
// ---------------------------------------------------------------------------------------
// Two of the map's rules cannot be forced deterministically through the GL surface - a
// cancelled node depends on beating a worker to it, and a CompileEnv re-capture needs a
// backend swap. Both are unconditional properties of the class, so they are asserted here
// against the class.

namespace {
    SharedPtr<ShaderCompileTask> MakeNode(const String& text, const ShaderStage stage,
                                          const SharedPtr<const MG_Util::ShaderTranspiler::CompileEnv>& env) {
        auto source = MakeShared<const String>(text);
        const Uint64 hash = ShaderPreprocessCache::HashSource(*source);
        return MakeShared<ShaderCompileTask>(stage, source, hash, env, nullptr, 0);
    }
} // namespace

TEST(ShaderCompileAdoptionMapTest, RegisteredNodeIsAdoptedOnAnExactMatch) {
    const auto& env = MG_Util::ShaderTranspiler::GetDefaultCompileEnv();
    ShaderCompileAdoptionMap map;
    const String text = "#version 460\nvoid main() {}\n";
    const SharedPtr<ShaderCompileTask> node = MakeNode(text, ShaderStage::Fragment, env);
    map.Register(node);

    EXPECT_EQ(map.FindAdoptable(ShaderStage::Fragment, ShaderPreprocessCache::HashSource(text), text,
                                env->fingerprint),
              node);
    EXPECT_EQ(map.GetAdoptionCount(), 1u);

    // Every discriminator in the key is load-bearing.
    EXPECT_EQ(map.FindAdoptable(ShaderStage::Vertex, ShaderPreprocessCache::HashSource(text), text,
                                env->fingerprint),
              nullptr);
    const String other = text + "\n";
    EXPECT_EQ(map.FindAdoptable(ShaderStage::Fragment, ShaderPreprocessCache::HashSource(other), other,
                                env->fingerprint),
              nullptr);
    EXPECT_EQ(map.GetAdoptionCount(), 1u) << "a miss must not count as an adoption";
}

// A memo must never be handed back under an environment other than the one it was computed
// against: the compute local-size verdict inside the pipeline reads CompileEnv's device
// limits, so a node captured under one backend's limits is not a valid answer under
// another's. The fingerprint is what enforces that, and it is part of the key.
TEST(ShaderCompileAdoptionMapTest, EnvFingerprintMismatchIsNotAdopted) {
    const auto& env = MG_Util::ShaderTranspiler::GetDefaultCompileEnv();
    ShaderCompileAdoptionMap map;
    const String text = "#version 460\nvoid main() {}\n";
    map.Register(MakeNode(text, ShaderStage::Fragment, env));

    // A genuinely different environment: different device limits, hence a different
    // fingerprint, hence a different key.
    auto otherEnv = MakeShared<MG_Util::ShaderTranspiler::CompileEnv>(*env);
    otherEnv->maxComputeWorkGroupInvocations = env->maxComputeWorkGroupInvocations + 1;
    otherEnv->fingerprint = MG_Util::ShaderTranspiler::ComputeCompileEnvFingerprint(*otherEnv);
    ASSERT_NE(otherEnv->fingerprint, env->fingerprint);

    EXPECT_EQ(map.FindAdoptable(ShaderStage::Fragment, ShaderPreprocessCache::HashSource(text), text,
                                otherEnv->fingerprint),
              nullptr);
    EXPECT_EQ(map.GetAdoptionCount(), 0u);
}

// A node that settled as Cancelled published nothing, so adopting it would hand the new
// object a compile that can only ever report GL_FALSE. It must be a miss, and the dead entry
// must be pruned where it is found rather than waiting for the amortized sweep.
TEST(ShaderCompileAdoptionMapTest, CancelledNodeIsNotAdoptedAndIsPruned) {
    const auto& env = MG_Util::ShaderTranspiler::GetDefaultCompileEnv();
    ShaderCompileAdoptionMap map;
    const String text = "#version 460\nvoid main() {}\n";
    const SharedPtr<ShaderCompileTask> node = MakeNode(text, ShaderStage::Fragment, env);
    map.Register(node);
    // Never posted, so this settles the node as Cancelled right here.
    node->Cancel();
    ASSERT_TRUE(node->IsCancelled());
    ASSERT_EQ(map.GetEntryCount(), 1u);

    EXPECT_EQ(map.FindAdoptable(ShaderStage::Fragment, ShaderPreprocessCache::HashSource(text), text,
                                env->fingerprint),
              nullptr);
    EXPECT_EQ(map.GetEntryCount(), 0u) << "the dead entry must be pruned on the lookup that found it";
    EXPECT_EQ(map.GetAdoptionCount(), 0u);
}

// The map is an index, never an owner: once the last real holder is gone the entry expires
// and is pruned, so a node's artifacts can never be kept alive by the map alone.
TEST(ShaderCompileAdoptionMapTest, ExpiredNodeIsNotAdoptedAndIsPruned) {
    const auto& env = MG_Util::ShaderTranspiler::GetDefaultCompileEnv();
    ShaderCompileAdoptionMap map;
    const String text = "#version 460\nvoid main() {}\n";
    {
        map.Register(MakeNode(text, ShaderStage::Fragment, env));
    }
    ASSERT_EQ(map.GetEntryCount(), 1u);

    EXPECT_EQ(map.FindAdoptable(ShaderStage::Fragment, ShaderPreprocessCache::HashSource(text), text,
                                env->fingerprint),
              nullptr);
    EXPECT_EQ(map.GetEntryCount(), 0u);
}

// The amortized sweep keeps the index O(live nodes) instead of O(compiles ever issued).
TEST(ShaderCompileAdoptionMapTest, SweepReclaimsDeadEntries) {
    const auto& env = MG_Util::ShaderTranspiler::GetDefaultCompileEnv();
    ShaderCompileAdoptionMap map;
    // Every one of these dies immediately, so nothing but dead weight accumulates - and the
    // map must not grow without bound because of it.
    for (SizeT i = 0; i < ShaderCompileAdoptionMap::kMinSweepThreshold * 4; ++i) {
        map.Register(MakeNode("#version 460\nvoid main() { float x" + std::to_string(i) + " = 0.0; }\n",
                              ShaderStage::Fragment, env));
    }
    EXPECT_LE(map.GetEntryCount(), ShaderCompileAdoptionMap::kMinSweepThreshold)
        << "expired entries must be reclaimed, not accumulated";
}
