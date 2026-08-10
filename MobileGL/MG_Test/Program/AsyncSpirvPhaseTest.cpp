// MobileGL - MobileGL/MG_Test/Program/AsyncSpirvPhaseTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The two-phase link: ProgramLinkTask (phase A - everything GL can be asked about the
// program) publishes through the existing join gate, and a chained ProgramSpirvTask (phase B -
// GlslangToSpv, spirv-opt, the global-UBO routing tables) publishes through a second gate that
// only five getters use.
//
// Four properties are under test, and they are the four the split can get wrong:
//   * phase A really is complete - LINK_STATUS, the info log and the WHOLE reflection surface
//     answer while phase B is still outstanding, and answering them does not settle it;
//   * a link that FAILS never posts phase B at all, and a phase B that never produced SPIR-V
//     leaves a program that is linked and queryable but not drawable;
//   * glUniform* writes taken inside the A->B window are replayed byte-for-byte at the phase-B
//     publish, with the UBO content version moving exactly when a direct write would have
//     moved it;
//   * both publishes bump the link-observable version counters, or a backend memo taken inside
//     the window survives the arrival of the SPIR-V.
//
// Like the other async suites, every case drives the real GL entry points and flips
// MG_Config::Features.AsyncShaderCompile itself, so the file behaves identically however the
// suite was launched.

#include <gtest/gtest.h>

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

    // One worker, restored on the way out. With a single worker a batch of links leaves phase-B
    // jobs queued behind each other, which is the whole window this suite needs to observe.
    class SingleWorkerScope {
    public:
        SingleWorkerScope() : m_saved(MG_Util::Async::ShaderCompilePool::Get().GetThreadCount()) {
            MG_Util::Async::ShaderCompilePool::Get().SetMaxConcurrency(1);
        }
        ~SingleWorkerScope() { MG_Util::Async::ShaderCompilePool::Get().SetMaxConcurrency(m_saved); }
        SingleWorkerScope(const SingleWorkerScope&) = delete;
        SingleWorkerScope& operator=(const SingleWorkerScope&) = delete;

    private:
        const Uint m_saved;
    };

    const char* kVs = R"(#version 460
layout(location = 0) in vec3 aPos;
out vec3 vPos;
void main() {
    vPos = aPos;
    gl_Position = vec4(aPos, 1.0);
}
)";

    // Heavy enough that neither the compile nor either link phase is instantaneous, and it
    // READS every uniform it declares so the optimizer cannot delete the global UBO out from
    // under the routing tables. Templated on an index so every instance is distinct source
    // text (no preprocess-memo hit).
    String MakeUniformSource(const int index) {
        const String n = std::to_string(index);
        String source = "#version 460\n";
        source += "in vec3 vPos;\n";
        source += "layout(location = 0) out vec4 fragColor;\n";
        source += "uniform mat4 uModel" + n + ";\n";
        source += "uniform vec3 uTint" + n + ";\n";
        source += "uniform float uArr" + n + "[4];\n";
        source += "uniform float uSeed" + n + ";\n";
        source += "void main() {\n";
        source += "    float acc = uSeed" + n + ";\n";
        for (int i = 0; i < 200; ++i) {
            source += "    acc = acc * 1.0001 + sin(acc + " + std::to_string(i) + ".0) * cos(acc);\n";
        }
        source += "    vec4 p = uModel" + n + " * vec4(vPos, 1.0);\n";
        source += "    acc += p.x + p.y + p.z + p.w;\n";
        source += "    acc += uArr" + n + "[0] + uArr" + n + "[1] + uArr" + n + "[2] + uArr" + n + "[3];\n";
        source += "    fragColor = vec4(uTint" + n + " * acc, 1.0);\n";
        source += "}\n";
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

    const SharedPtr<MG_State::GLState::ProgramObject>& Object(const GLuint program) {
        return MG_State::pGLContext->GetProgramObject(program);
    }

    Bool SpirvIsSettled(const GLuint program) {
        const auto& object = Object(program);
        return object == nullptr || object->IsSpirvComplete();
    }

    Vector<Uint64> SpirvDigest(const GLuint program) {
        Vector<Uint64> digest;
        const auto& object = Object(program);
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

    // A batch of linked programs, all with phase A joined and (for most of them) phase B still
    // outstanding. Returns the GL names in link order; `indices` receives the source index used
    // for each, so uniform names can be reconstructed.
    Vector<GLuint> LinkBatch(const int count, const int firstIndex, Vector<String>& sourceStorage) {
        const GLuint vs = MakeShader(GL_VERTEX_SHADER, kVs);
        Vector<GLuint> programs;
        programs.reserve(static_cast<SizeT>(count));
        for (int i = 0; i < count; ++i) {
            sourceStorage.push_back(MakeUniformSource(firstIndex + i));
            const char* text = sourceStorage.back().c_str();
            const GLuint fs = CreateShader(GL_FRAGMENT_SHADER);
            ShaderSource(fs, 1, &text, nullptr);
            CompileShader(fs);
            const GLuint program = CreateProgram();
            AttachShader(program, vs);
            AttachShader(program, fs);
            LinkProgram(program);
            programs.push_back(program);
        }
        return programs;
    }

    class AsyncSpirvPhaseTest : public ::testing::Test {
    protected:
        void SetUp() override { MobileGL::Initialize(); }
    };
} // namespace

// ---------------------------------------------------------------------------------------
// Phase A is complete on its own
// ---------------------------------------------------------------------------------------

// The headline property: the whole GL query surface is answerable out of phase A. Every query
// below is asked while phase-B jobs are still queued, and none of them may settle one.
TEST_F(AsyncSpirvPhaseTest, EveryReflectionQueryAnswersWhileTheSpirvJobIsOutstanding) {
    const AsyncModeScope async(true);
    const SingleWorkerScope oneWorker;
    constexpr int kPrograms = 24;
    constexpr int kFirst = 31000;

    Vector<String> sources;
    const Vector<GLuint> programs = LinkBatch(kPrograms, kFirst, sources);

    int outstanding = 0;
    for (int i = 0; i < kPrograms; ++i) {
        const GLuint program = programs[static_cast<SizeT>(i)];
        const String n = std::to_string(kFirst + i);

        // LINK_STATUS and the info log: phase A.
        EXPECT_EQ(QueryLinkStatus(program), GL_TRUE) << QueryProgramInfoLog(program);

        // Uniform locations, including the array's element slots.
        const GLint locModel = GetUniformLocation(program, ("uModel" + n).c_str());
        const GLint locTint = GetUniformLocation(program, ("uTint" + n).c_str());
        const GLint locArr = GetUniformLocation(program, ("uArr" + n + "[0]").c_str());
        const GLint locArr2 = GetUniformLocation(program, ("uArr" + n + "[2]").c_str());
        EXPECT_GE(locModel, 0);
        EXPECT_GE(locTint, 0);
        EXPECT_GE(locArr, 0);
        EXPECT_EQ(locArr2, locArr + 2);

        // Counts and name lengths.
        GLint activeUniforms = 0;
        GLint maxNameLength = 0;
        GLint activeAttributes = 0;
        GLint activeBlocks = 0;
        GetProgramiv(program, GL_ACTIVE_UNIFORMS, &activeUniforms);
        GetProgramiv(program, GL_ACTIVE_UNIFORM_MAX_LENGTH, &maxNameLength);
        GetProgramiv(program, GL_ACTIVE_ATTRIBUTES, &activeAttributes);
        GetProgramiv(program, GL_ACTIVE_UNIFORM_BLOCKS, &activeBlocks);
        EXPECT_GE(activeUniforms, 4);
        EXPECT_GT(maxNameLength, 0);
        EXPECT_GE(activeAttributes, 1);
        EXPECT_EQ(activeBlocks, 0); // the synthesized global UBO is not GL-visible

        // Per-uniform reflection.
        std::vector<GLchar> nameBuffer(static_cast<size_t>(maxNameLength) + 1);
        GLsizei written = 0;
        GLint size = 0;
        GLenum type = 0;
        GetActiveUniform(program, 0, maxNameLength, &written, &size, &type, nameBuffer.data());
        EXPECT_GT(written, 0);

        // Attributes and fragment outputs.
        EXPECT_GE(GetAttribLocation(program, "aPos"), 0);
        EXPECT_GE(GetFragDataLocation(program, "fragColor"), 0);

        // Transform feedback and the geometry input type, both phase A.
        GLint xfbVaryings = -1;
        GetProgramiv(program, GL_TRANSFORM_FEEDBACK_VARYINGS, &xfbVaryings);
        EXPECT_EQ(xfbVaryings, 0);
        EXPECT_EQ(GetError(), GL_NO_ERROR);

        if (!SpirvIsSettled(program)) ++outstanding;
    }

    EXPECT_GT(outstanding, 0) << "every phase-B job had already drained while the whole query surface was being "
                                 "read - one of those queries is joining phase B";
}

// ---------------------------------------------------------------------------------------
// A failed link never posts phase B
// ---------------------------------------------------------------------------------------

TEST_F(AsyncSpirvPhaseTest, ALinkThatFailsNeverProducesSpirv) {
    for (const Bool async : {false, true}) {
        const AsyncModeScope scope(async);

        const char* brokenFs = R"(#version 460
layout(location = 0) out vec4 fragColor;
void main() { fragColor = thisIdentifierWasNeverDeclared; }
)";
        const GLuint vs = MakeShader(GL_VERTEX_SHADER, kVs);
        const GLuint fs = MakeShader(GL_FRAGMENT_SHADER, brokenFs);
        const GLuint program = CreateProgram();
        AttachShader(program, vs);
        AttachShader(program, fs);
        LinkProgram(program);

        EXPECT_EQ(QueryLinkStatus(program), GL_FALSE);
        EXPECT_FALSE(QueryProgramInfoLog(program).empty());

        const auto& object = Object(program);
        ASSERT_NE(object, nullptr);
        // Phase B settled (as cancelled) rather than being left in flight, so nothing can
        // block on it later, and it published nothing.
        EXPECT_TRUE(object->IsSpirvComplete());
        EXPECT_FALSE(object->GetSpirvStatus());
        EXPECT_TRUE(object->GetGeneratedSpirv().empty());
        EXPECT_EQ(GetError(), GL_NO_ERROR);
    }
}

// A fragment output past GL_MAX_DRAW_BUFFERS fails the link inside phase A, and it is the
// check that used to run AFTER 68 s of SPIR-V work per pack load.
TEST_F(AsyncSpirvPhaseTest, AFragmentOutputRangeFailureIsDecidedInPhaseA) {
    const AsyncModeScope async(true);

    const char* fs = R"(#version 460
layout(location = 4096) out vec4 fragColor;
void main() { fragColor = vec4(1.0); }
)";
    const GLuint vs = MakeShader(GL_VERTEX_SHADER, kVs);
    const GLuint fsId = MakeShader(GL_FRAGMENT_SHADER, fs);
    const GLuint program = CreateProgram();
    AttachShader(program, vs);
    AttachShader(program, fsId);
    LinkProgram(program);

    EXPECT_EQ(QueryLinkStatus(program), GL_FALSE) << "a fragment output past GL_MAX_DRAW_BUFFERS must fail the link";
    const auto& object = Object(program);
    ASSERT_NE(object, nullptr);
    EXPECT_TRUE(object->IsSpirvComplete());
    EXPECT_FALSE(object->GetSpirvStatus());
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// ---------------------------------------------------------------------------------------
// glUniform* across the window
// ---------------------------------------------------------------------------------------

// The write path's half of the split: a non-opaque glUniform* inside the A->B window is
// recorded rather than joined, and the bytes that come back afterwards are the bytes that
// went in. Writes are made through glProgramUniform* so no program has to be current.
TEST_F(AsyncSpirvPhaseTest, UniformWritesInsideTheWindowReplayExactly) {
    const AsyncModeScope async(true);
    const SingleWorkerScope oneWorker;
    constexpr int kPrograms = 24;
    constexpr int kFirst = 32000;

    Vector<String> sources;
    const Vector<GLuint> programs = LinkBatch(kPrograms, kFirst, sources);

    struct Expectation {
        GLuint program = 0;
        GLint locModel = -1;
        GLint locTint = -1;
        GLint locArr = -1;
        GLfloat model[16] = {};
        GLfloat tint[3] = {};
        GLfloat arr1 = 0.0f;
        Bool wasBuffered = false;
    };
    Vector<Expectation> expectations;

    int buffered = 0;
    for (int i = 0; i < kPrograms; ++i) {
        Expectation e;
        e.program = programs[static_cast<SizeT>(i)];
        const String n = std::to_string(kFirst + i);
        ASSERT_EQ(QueryLinkStatus(e.program), GL_TRUE) << QueryProgramInfoLog(e.program);
        // Sampled straight after the LINK_STATUS read: phase A is settled, phase B usually is
        // not, and this is exactly the window an application writes its uniforms in.
        e.wasBuffered = !SpirvIsSettled(e.program);
        if (e.wasBuffered) ++buffered;

        e.locModel = GetUniformLocation(e.program, ("uModel" + n).c_str());
        e.locTint = GetUniformLocation(e.program, ("uTint" + n).c_str());
        e.locArr = GetUniformLocation(e.program, ("uArr" + n + "[0]").c_str());
        ASSERT_GE(e.locModel, 0);
        ASSERT_GE(e.locTint, 0);
        ASSERT_GE(e.locArr, 0);

        for (int c = 0; c < 16; ++c) e.model[c] = static_cast<GLfloat>(i) + static_cast<GLfloat>(c) * 0.25f;
        e.tint[0] = 0.125f * static_cast<GLfloat>(i);
        e.tint[1] = 0.25f * static_cast<GLfloat>(i);
        e.tint[2] = 0.5f * static_cast<GLfloat>(i);
        e.arr1 = 7.5f + static_cast<GLfloat>(i);

        ProgramUniformMatrix4fv(e.program, e.locModel, 1, GL_FALSE, e.model);
        ProgramUniform3fv(e.program, e.locTint, 1, e.tint);
        // An element in the middle of an array, addressed by its own location.
        ProgramUniform1fv(e.program, e.locArr + 1, 1, &e.arr1);
        // Last write wins: overwrite the tint, so the replay's ordering is under test too.
        e.tint[1] = 0.75f;
        ProgramUniform3fv(e.program, e.locTint, 1, e.tint);

        expectations.push_back(e);
    }

    EXPECT_GT(buffered, 0) << "no write ever landed inside the A->B window; this case proved nothing";

    for (const Expectation& e : expectations) {
        GLfloat model[16] = {};
        GLfloat tint[3] = {};
        GLfloat arr1 = 0.0f;
        GetUniformfv(e.program, e.locModel, model);
        GetUniformfv(e.program, e.locTint, tint);
        GetUniformfv(e.program, e.locArr + 1, &arr1);
        for (int c = 0; c < 16; ++c) {
            EXPECT_FLOAT_EQ(model[c], e.model[c]) << "program " << e.program << " matrix component " << c;
        }
        for (int c = 0; c < 3; ++c) {
            EXPECT_FLOAT_EQ(tint[c], e.tint[c]) << "program " << e.program << " tint component " << c;
        }
        EXPECT_FLOAT_EQ(arr1, e.arr1) << "program " << e.program << " array element 1";
        // Reading them settled phase B, so the program is drawable now.
        const auto& object = Object(e.program);
        ASSERT_NE(object, nullptr);
        EXPECT_TRUE(object->GetSpirvStatus());
    }
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// The dedupe property the live write path has, preserved across the detour: replaying a record
// that really changes bytes moves the UBO content version, and a bytes-identical write made
// AFTER the replay does not move it. Both matter - the first is what makes a backend re-upload
// a UBO it cached during the window, the second is what stops Minecraft's per-frame re-set of
// identical matrices from re-uploading every frame.
TEST_F(AsyncSpirvPhaseTest, TheReplayMovesTheUboContentVersionExactlyLikeADirectWrite) {
    const AsyncModeScope async(true);
    const SingleWorkerScope oneWorker;
    constexpr int kPrograms = 24;
    constexpr int kFirst = 33000;

    Vector<String> sources;
    const Vector<GLuint> programs = LinkBatch(kPrograms, kFirst, sources);

    int checked = 0;
    for (int i = 0; i < kPrograms; ++i) {
        const GLuint program = programs[static_cast<SizeT>(i)];
        const String n = std::to_string(kFirst + i);
        ASSERT_EQ(QueryLinkStatus(program), GL_TRUE) << QueryProgramInfoLog(program);
        if (SpirvIsSettled(program)) continue; // phase B already landed; nothing to buffer

        const auto& object = Object(program);
        ASSERT_NE(object, nullptr);
        const GLint locTint = GetUniformLocation(program, ("uTint" + n).c_str());
        ASSERT_GE(locTint, 0);

        const Uint32 versionBeforeWrite = object->GetUBOContentVersion();
        const GLfloat tint[3] = {0.5f, 0.25f, 0.125f};
        ProgramUniform3fv(program, locTint, 1, tint);
        // Still buffered: nothing has been written into the shadow yet, so the content version
        // cannot have moved.
        EXPECT_EQ(object->GetUBOContentVersion(), versionBeforeWrite)
            << "a buffered write must not move the content version before it is replayed";

        // The join replays it - and the replay writes real bytes, so it moves.
        object->JoinLinkAndSpirv();
        EXPECT_NE(object->GetUBOContentVersion(), versionBeforeWrite)
            << "the replayed write changed bytes, so the content version had to move";

        // And now the ordinary dedupe applies again.
        const Uint32 versionAfterReplay = object->GetUBOContentVersion();
        ProgramUniform3fv(program, locTint, 1, tint);
        EXPECT_EQ(object->GetUBOContentVersion(), versionAfterReplay)
            << "a bytes-identical rewrite after the replay must not move the content version";
        ++checked;
    }

    EXPECT_GT(checked, 0) << "no program was ever observed inside the A->B window";
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// ---------------------------------------------------------------------------------------
// Version counters
// ---------------------------------------------------------------------------------------

// Both publishes bump the link-observable versions. Without the second bump, a backend memo
// taken inside the A->B window - when the program already answers as linked but has no SPIR-V
// and no uniform shadow - would survive the arrival of both.
TEST_F(AsyncSpirvPhaseTest, TheSpirvPublishBumpsTheLinkObservableVersions) {
    const AsyncModeScope async(true);
    const SingleWorkerScope oneWorker;
    constexpr int kPrograms = 24;
    constexpr int kFirst = 34000;

    Vector<String> sources;
    const Vector<GLuint> programs = LinkBatch(kPrograms, kFirst, sources);

    int checked = 0;
    for (int i = 0; i < kPrograms; ++i) {
        const GLuint program = programs[static_cast<SizeT>(i)];
        ASSERT_EQ(QueryLinkStatus(program), GL_TRUE) << QueryProgramInfoLog(program);
        if (SpirvIsSettled(program)) continue;

        const auto& object = Object(program);
        ASSERT_NE(object, nullptr);
        const Uint32 backendVersionInWindow = object->GetBackendStateVersion();
        const Uint32 linkVersionInWindow = object->GetLinkVersion();

        object->JoinLinkAndSpirv();

        EXPECT_NE(object->GetBackendStateVersion(), backendVersionInWindow)
            << "a memo keyed on backendStateVersion inside the window would have survived the SPIR-V publish";
        EXPECT_NE(object->GetLinkVersion(), linkVersionInWindow);
        ++checked;
    }

    EXPECT_GT(checked, 0) << "no program was ever observed inside the A->B window";
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// ---------------------------------------------------------------------------------------
// Cancel
// ---------------------------------------------------------------------------------------

// A relink over a program whose phase B is still in flight drops that phase B where it stands
// and the new link answers for itself. The half-published program the old one-handler-per-link
// comment warned about is structurally impossible: the relink resets BOTH halves.
TEST_F(AsyncSpirvPhaseTest, RelinkingOverAPendingSpirvJobIsClean) {
    const AsyncModeScope async(true);
    const SingleWorkerScope oneWorker;
    constexpr int kPrograms = 16;
    constexpr int kFirst = 35000;

    Vector<String> sources;
    const Vector<GLuint> programs = LinkBatch(kPrograms, kFirst, sources);

    int relinked = 0;
    for (int i = 0; i < kPrograms; ++i) {
        const GLuint program = programs[static_cast<SizeT>(i)];
        ASSERT_EQ(QueryLinkStatus(program), GL_TRUE) << QueryProgramInfoLog(program);
        if (SpirvIsSettled(program)) continue;

        // Relink while phase B is queued.
        LinkProgram(program);
        ++relinked;

        EXPECT_EQ(QueryLinkStatus(program), GL_TRUE) << QueryProgramInfoLog(program);
        const auto& object = Object(program);
        ASSERT_NE(object, nullptr);
        object->JoinLinkAndSpirv();
        EXPECT_TRUE(object->GetSpirvStatus()) << "the relink's own phase B must have produced SPIR-V";
        EXPECT_FALSE(object->GetGeneratedSpirv().empty());
    }

    EXPECT_GT(relinked, 0) << "no relink ever landed inside the A->B window";
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// Destroying a program whose phase B is still queued must not wait for it, and must not leave
// anything behind that a later join could block on.
TEST_F(AsyncSpirvPhaseTest, DeletingAProgramWithAPendingSpirvJobDoesNotBlock) {
    const AsyncModeScope async(true);
    const SingleWorkerScope oneWorker;
    constexpr int kPrograms = 16;
    constexpr int kFirst = 36000;

    Vector<String> sources;
    const Vector<GLuint> programs = LinkBatch(kPrograms, kFirst, sources);

    int deleted = 0;
    for (int i = 0; i < kPrograms; ++i) {
        const GLuint program = programs[static_cast<SizeT>(i)];
        ASSERT_EQ(QueryLinkStatus(program), GL_TRUE) << QueryProgramInfoLog(program);
        if (!SpirvIsSettled(program)) ++deleted;
        DeleteProgram(program);
    }

    EXPECT_GT(deleted, 0) << "no program was deleted inside the A->B window";
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// ---------------------------------------------------------------------------------------
// Inline equivalence
// ---------------------------------------------------------------------------------------

// The contract the async-off path has always had: it is byte-identical to the synchronous
// implementation. The split runs two bodies instead of one, in order, on the calling thread -
// and the artifacts it produces must equal what the asynchronous path produces.
TEST_F(AsyncSpirvPhaseTest, AsyncOffAndAsyncOnProduceIdenticalSpirvAndShadow) {
    Vector<Uint64> syncDigest;
    Uint syncUboSize = 0;
    Vector<Uint> syncOffsets;

    const auto buildOnce = [&](const Bool async, Vector<Uint64>& digest, Uint& uboSize, Vector<Uint>& offsets) {
        const AsyncModeScope scope(async);
        const String source = MakeUniformSource(37000);
        const char* text = source.c_str();
        const GLuint vs = MakeShader(GL_VERTEX_SHADER, kVs);
        const GLuint fs = CreateShader(GL_FRAGMENT_SHADER);
        ShaderSource(fs, 1, &text, nullptr);
        CompileShader(fs);
        const GLuint program = CreateProgram();
        AttachShader(program, vs);
        AttachShader(program, fs);
        LinkProgram(program);
        ASSERT_EQ(QueryLinkStatus(program), GL_TRUE) << QueryProgramInfoLog(program);

        const auto& object = Object(program);
        ASSERT_NE(object, nullptr);
        if (!async) {
            // The whole point of the mode: nothing is outstanding when glLinkProgram returns.
            EXPECT_TRUE(object->IsLinkComplete());
        }
        digest = SpirvDigest(program);
        uboSize = object->GetUBOSize();
        for (Uint location = 0; location <= object->GetMaxUniformLocation(); ++location) {
            offsets.push_back(object->GetUniformOffset(location));
        }
        EXPECT_TRUE(object->GetSpirvStatus());
    };

    buildOnce(false, syncDigest, syncUboSize, syncOffsets);
    ASSERT_FALSE(syncDigest.empty());

    Vector<Uint64> asyncDigest;
    Uint asyncUboSize = 0;
    Vector<Uint> asyncOffsets;
    buildOnce(true, asyncDigest, asyncUboSize, asyncOffsets);

    EXPECT_EQ(asyncDigest, syncDigest) << "the two modes produced different SPIR-V";
    EXPECT_EQ(asyncUboSize, syncUboSize);
    EXPECT_EQ(asyncOffsets, syncOffsets);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}
