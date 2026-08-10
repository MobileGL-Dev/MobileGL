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
        // An OPAQUE uniform, so the window cases can pin the "glUniform1i(samplerLoc, unit)
        // right after a link is a zero-join operation" claim the split is built around.
        source += "uniform sampler2D uTex" + n + ";\n";
        source += "void main() {\n";
        source += "    float acc = uSeed" + n + ";\n";
        for (int i = 0; i < 200; ++i) {
            source += "    acc = acc * 1.0001 + sin(acc + " + std::to_string(i) + ".0) * cos(acc);\n";
        }
        source += "    vec4 p = uModel" + n + " * vec4(vPos, 1.0);\n";
        source += "    acc += p.x + p.y + p.z + p.w;\n";
        source += "    acc += uArr" + n + "[0] + uArr" + n + "[1] + uArr" + n + "[2] + uArr" + n + "[3];\n";
        source += "    vec4 t = texture(uTex" + n + ", vPos.xy);\n";
        source += "    fragColor = vec4(uTint" + n + " * acc, 1.0) * t;\n";
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
        // GetSpirvStatus() FIRST, and the order is load-bearing rather than stylistic: phase B
        // is settled by a continuation that runs on whichever worker drove phase A terminal,
        // and JobNode::TryTransition releases waiters (notify_all) BEFORE it runs its
        // continuation list - so the GL thread can be back here with phase A published while
        // that one-line lambda has not run yet. GetSpirvStatus() goes through the phase-B
        // gate, which Waits; only after it has can IsSpirvComplete() be asserted without a
        // race. Asserting the other way round is a rare CI flake, not a red.
        EXPECT_FALSE(object->GetSpirvStatus());
        EXPECT_TRUE(object->IsSpirvComplete());
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
    // GetSpirvStatus() before IsSpirvComplete(); see ALinkThatFailsNeverProducesSpirv.
    EXPECT_FALSE(object->GetSpirvStatus());
    EXPECT_TRUE(object->IsSpirvComplete());
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// ---------------------------------------------------------------------------------------
// Linked, but the SPIR-V never arrived
// ---------------------------------------------------------------------------------------

// The state the split invented and GL gives no way out of: phase A published LINK_STATUS
// GL_TRUE, and phase B then settled CANCELLED rather than Complete - its body threw
// (bad_alloc out of GlslangToSpv/spirv-opt under pack-load memory pressure), the pool failed
// to enqueue it, or teardown cancelled it while it was queued. The shadow is then a
// default-constructed SpirvArtifacts: empty uniformOffsets, null scratch, spirvStatus false -
// while the whole phase-A query surface, IsValidUniformLocation() included, keeps answering.
//
// Every getter and every entry point on that surface must degrade, not fault. Before the
// bounds check in GetUniformOffset this was a null dereference on the first glUniform* or
// glGetUniform* the application made.
//
// The state is produced deterministically rather than by racing a cancel: after the
// LINK_STATUS read has published phase A (m_pendingLink is null), CancelLink() can only reach
// the SPIR-V job - which is exactly the shape StopAndDrain produces for a phase B queued
// behind an already-complete phase A.
TEST_F(AsyncSpirvPhaseTest, ALinkedProgramWhoseSpirvJobWasCancelledDegradesInsteadOfFaulting) {
    const AsyncModeScope async(true);
    const SingleWorkerScope oneWorker;
    constexpr int kPrograms = 12;
    constexpr int kFirst = 38000;

    Vector<String> sources;
    const Vector<GLuint> programs = LinkBatch(kPrograms, kFirst, sources);

    int cancelled = 0;
    for (int i = 0; i < kPrograms; ++i) {
        const GLuint program = programs[static_cast<SizeT>(i)];
        const String n = std::to_string(kFirst + i);
        ASSERT_EQ(QueryLinkStatus(program), GL_TRUE) << QueryProgramInfoLog(program);
        const auto& object = Object(program);
        ASSERT_NE(object, nullptr);
        if (object->IsSpirvComplete()) continue; // already published; not the state under test

        object->CancelLink(); // phase A is published, so this reaches only the SPIR-V job
        ++cancelled;

        // ---- the contract: linked, fully queryable, not drawable ----
        EXPECT_TRUE(object->GetLinkStatus()) << "a cancelled phase B must not retract LINK_STATUS";
        EXPECT_EQ(QueryLinkStatus(program), GL_TRUE);
        EXPECT_FALSE(object->GetSpirvStatus());
        EXPECT_TRUE(object->IsSpirvComplete());
        // This is the expression both backends' bind gates evaluate.
        EXPECT_FALSE(object->GetLinkStatus() && object->GetSpirvStatus())
            << "the backends must refuse to bind a program with no SPIR-V";
        EXPECT_TRUE(object->GetGeneratedSpirv().empty());
        EXPECT_EQ(object->GetUBOSize(), 0u);
        EXPECT_EQ(object->GetUBOData(), nullptr);

        // ---- the reflection surface still answers ----
        const GLint locModel = GetUniformLocation(program, ("uModel" + n).c_str());
        const GLint locTint = GetUniformLocation(program, ("uTint" + n).c_str());
        const GLint locTex = GetUniformLocation(program, ("uTex" + n).c_str());
        ASSERT_GE(locModel, 0);
        ASSERT_GE(locTint, 0);
        ASSERT_GE(locTex, 0);
        EXPECT_TRUE(object->IsValidUniformLocation(locTint));

        // ---- and every write/read path degrades ----
        // Direct getter first: kInvalidUniformOffset, not an out-of-bounds index.
        EXPECT_EQ(object->GetUniformOffset(static_cast<Uint>(locTint)),
                  MG_State::GLState::ProgramObject::kInvalidUniformOffset);
        EXPECT_EQ(object->GetUniformOffset(object->GetMaxUniformLocation()),
                  MG_State::GLState::ProgramObject::kInvalidUniformOffset);

        const GLfloat tint[3] = {1.0f, 2.0f, 3.0f};
        GLfloat model[16] = {};
        for (int c = 0; c < 16; ++c) model[c] = static_cast<GLfloat>(c);
        ProgramUniform3fv(program, locTint, 1, tint);                 // dropped, not faulted
        ProgramUniformMatrix4fv(program, locModel, 1, GL_FALSE, model); // ditto
        UseProgram(program);
        Uniform3fv(locTint, 1, tint); // the glUseProgram + glUniform* entry, same verdict
        UseProgram(0);

        // The opaque branch never touches phase B, so it keeps working in full.
        ProgramUniform1i(program, locTex, 3);
        GLint unit = -1;
        GetUniformiv(program, locTex, &unit);
        EXPECT_EQ(unit, 3) << "sampler units are phase-A state and must survive a lost phase B";

        // Reads leave the caller's buffer alone rather than faulting.
        GLfloat readback[16] = {};
        for (int c = 0; c < 16; ++c) readback[c] = -1.0f;
        GetUniformfv(program, locModel, readback);
        for (int c = 0; c < 16; ++c) {
            EXPECT_FLOAT_EQ(readback[c], -1.0f) << "a program with no shadow must not write the query buffer";
        }
        EXPECT_EQ(GetError(), GL_NO_ERROR);
    }

    EXPECT_GT(cancelled, 0) << "no phase B was ever cancelled; this case proved nothing";
}

// The mirror image: with async off the state is not reachable at all, because both bodies run
// inline before glLinkProgram returns. Worth pinning - it is what makes the async-off mode a
// usable fallback for a device where the state above would be a problem.
TEST_F(AsyncSpirvPhaseTest, AsyncOffNeverProducesALinkedProgramWithoutSpirv) {
    const AsyncModeScope async(false);
    Vector<String> sources;
    const Vector<GLuint> programs = LinkBatch(4, 39000, sources);
    for (const GLuint program : programs) {
        const auto& object = Object(program);
        ASSERT_NE(object, nullptr);
        EXPECT_EQ(QueryLinkStatus(program), GL_TRUE) << QueryProgramInfoLog(program);
        EXPECT_TRUE(object->IsSpirvComplete()) << "nothing may be outstanding when async is off";
        EXPECT_TRUE(object->GetSpirvStatus());
        EXPECT_GT(object->GetUBOSize(), 0u);
    }
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
        GLint locTex = -1;
        GLfloat model[16] = {};
        GLfloat tint[3] = {};
        GLfloat arr1 = 0.0f;
        GLint texUnit = 0;
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
        e.locTex = GetUniformLocation(e.program, ("uTex" + n).c_str());
        ASSERT_GE(e.locModel, 0);
        ASSERT_GE(e.locTint, 0);
        ASSERT_GE(e.locArr, 0);
        ASSERT_GE(e.locTex, 0);

        for (int c = 0; c < 16; ++c) e.model[c] = static_cast<GLfloat>(i) + static_cast<GLfloat>(c) * 0.25f;
        e.tint[0] = 0.125f * static_cast<GLfloat>(i);
        e.tint[1] = 0.25f * static_cast<GLfloat>(i);
        e.tint[2] = 0.5f * static_cast<GLfloat>(i);
        e.arr1 = 7.5f + static_cast<GLfloat>(i);
        e.texUnit = i % 8;

        // An OPAQUE write inside the window. The design's load-bearing claim is that this is a
        // ZERO-JOIN operation - a sampler unit is phase-A state - and it is what Iris does
        // immediately after every glLinkProgram, so it is asserted rather than assumed.
        ProgramUniform1i(e.program, e.locTex, e.texUnit);
        if (e.wasBuffered) {
            EXPECT_FALSE(SpirvIsSettled(e.program))
                << "glUniform1i on a sampler must not settle phase B (program " << e.program << ")";
        }

        ProgramUniformMatrix4fv(e.program, e.locModel, 1, GL_FALSE, e.model);
        ProgramUniform3fv(e.program, e.locTint, 1, e.tint);
        // An element in the middle of an array, addressed by its own location.
        ProgramUniform1fv(e.program, e.locArr + 1, 1, &e.arr1);
        // Last write wins, and through the OTHER entry point for half the programs: the
        // Minecraft/Iris shape is glUseProgram + glUniform*, which reaches Uniform_State
        // through Uniformv_State/GetProgramForUniform rather than through the by-name form.
        e.tint[1] = 0.75f;
        if ((i % 2) == 0) {
            ProgramUniform3fv(e.program, e.locTint, 1, e.tint);
        } else {
            UseProgram(e.program);
            Uniform3fv(e.locTint, 1, e.tint);
            UseProgram(0);
        }
        if (e.wasBuffered) {
            EXPECT_FALSE(SpirvIsSettled(e.program))
                << "no glUniform* entry point may settle phase B (program " << e.program << ")";
        }

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
        GLint unit = -1;
        GetUniformiv(e.program, e.locTex, &unit);
        EXPECT_EQ(unit, e.texUnit) << "program " << e.program << " sampler unit";
        // Reading them settled phase B, so the program is drawable now.
        const auto& object = Object(e.program);
        ASSERT_NE(object, nullptr);
        EXPECT_TRUE(object->GetSpirvStatus());
    }
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// The overflow valve. BufferUniformWrite declines past kMaxBufferedUniformBytes and the
// caller falls through to the direct path - which JOINS, and therefore has to replay
// everything already buffered BEFORE performing its own write, or last-write-wins breaks for
// every uniform touched after the valve trips.
TEST_F(AsyncSpirvPhaseTest, TheBufferedWriteValveFallsThroughToADirectWriteWithoutLosingOrder) {
    const AsyncModeScope async(true);
    const SingleWorkerScope oneWorker;
    constexpr int kPrograms = 8;
    constexpr int kFirst = 40000;
    // kMaxBufferedUniformBytes is 4 MiB and one mat4 write buffers 4 columns x 16 bytes, so
    // this many calls is comfortably past the valve.
    constexpr int kFloodWrites = 80000;

    Vector<String> sources;
    const Vector<GLuint> programs = LinkBatch(kPrograms, kFirst, sources);

    int flooded = 0;
    for (int i = 0; i < kPrograms; ++i) {
        const GLuint program = programs[static_cast<SizeT>(i)];
        const String n = std::to_string(kFirst + i);
        ASSERT_EQ(QueryLinkStatus(program), GL_TRUE) << QueryProgramInfoLog(program);
        if (SpirvIsSettled(program)) continue;
        ++flooded;

        const GLint locModel = GetUniformLocation(program, ("uModel" + n).c_str());
        const GLint locTint = GetUniformLocation(program, ("uTint" + n).c_str());
        ASSERT_GE(locModel, 0);
        ASSERT_GE(locTint, 0);

        // A distinctive early value that must survive the valve: it is buffered, and the
        // fall-through write has to replay it before writing its own bytes.
        const GLfloat earlyTint[3] = {11.0f, 22.0f, 33.0f};
        ProgramUniform3fv(program, locTint, 1, earlyTint);

        GLfloat model[16] = {};
        for (int w = 0; w < kFloodWrites; ++w) {
            for (int c = 0; c < 16; ++c) model[c] = static_cast<GLfloat>(w) + static_cast<GLfloat>(c);
            ProgramUniformMatrix4fv(program, locModel, 1, GL_FALSE, model);
        }

        // Falling through joined, so the shadow is live and holds BOTH the buffered early
        // write and the last direct one.
        EXPECT_TRUE(SpirvIsSettled(program)) << "the valve must have fallen through to a joining write";
        GLfloat tintReadback[3] = {};
        GLfloat modelReadback[16] = {};
        GetUniformfv(program, locTint, tintReadback);
        GetUniformfv(program, locModel, modelReadback);
        for (int c = 0; c < 3; ++c) {
            EXPECT_FLOAT_EQ(tintReadback[c], earlyTint[c])
                << "the pre-valve buffered write was lost at component " << c;
        }
        for (int c = 0; c < 16; ++c) {
            EXPECT_FLOAT_EQ(modelReadback[c], static_cast<GLfloat>(kFloodWrites - 1) + static_cast<GLfloat>(c))
                << "last-write-wins broke across the valve at component " << c;
        }
    }

    EXPECT_GT(flooded, 0) << "no program was ever observed inside the A->B window";
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
    constexpr int kRelinkOffset = 500; // distinct uniform names for the second link

    // Built by hand rather than through LinkBatch, because the relink has to use DIFFERENT
    // source: relinking byte-identical source cannot tell the second link's artifacts from
    // the first's, so it would pass even if Link()'s prologue stopped resetting m_spirv.
    const GLuint vs = MakeShader(GL_VERTEX_SHADER, kVs);
    Vector<String> firstSources;
    Vector<String> secondSources;
    Vector<GLuint> programs;
    Vector<GLuint> fragmentShaders;
    for (int i = 0; i < kPrograms; ++i) {
        firstSources.push_back(MakeUniformSource(kFirst + i));
        const char* text = firstSources.back().c_str();
        const GLuint fs = CreateShader(GL_FRAGMENT_SHADER);
        ShaderSource(fs, 1, &text, nullptr);
        CompileShader(fs);
        const GLuint program = CreateProgram();
        AttachShader(program, vs);
        AttachShader(program, fs);
        LinkProgram(program);
        programs.push_back(program);
        fragmentShaders.push_back(fs);
    }

    int relinked = 0;
    for (int i = 0; i < kPrograms; ++i) {
        const GLuint program = programs[static_cast<SizeT>(i)];
        const String firstName = "uTint" + std::to_string(kFirst + i);
        ASSERT_EQ(QueryLinkStatus(program), GL_TRUE) << QueryProgramInfoLog(program);
        if (SpirvIsSettled(program)) continue;

        // A write buffered against link #1, which the relink must DROP. If CancelLink stopped
        // clearing the buffers, these bytes would be replayed into link #2's shadow at
        // whatever offset its own routing tables assigned - silently overwriting a uniform
        // GL 4.6 core 7.6 requires a relink to have reset to zero.
        const GLint firstTint = GetUniformLocation(program, firstName.c_str());
        ASSERT_GE(firstTint, 0);
        const GLfloat poison[3] = {123.0f, 456.0f, 789.0f};
        ProgramUniform3fv(program, firstTint, 1, poison);
        ASSERT_FALSE(SpirvIsSettled(program)) << "the poison write should have been buffered, not applied";

        // Relink, with different source, while phase B is queued.
        secondSources.push_back(MakeUniformSource(kFirst + kRelinkOffset + i));
        const char* secondText = secondSources.back().c_str();
        ShaderSource(fragmentShaders[static_cast<SizeT>(i)], 1, &secondText, nullptr);
        CompileShader(fragmentShaders[static_cast<SizeT>(i)]);
        LinkProgram(program);
        ++relinked;

        EXPECT_EQ(QueryLinkStatus(program), GL_TRUE) << QueryProgramInfoLog(program);
        // The artifacts really are the SECOND link's: the first link's uniform is gone.
        EXPECT_EQ(GetUniformLocation(program, firstName.c_str()), -1)
            << "the relink is still answering out of the previous link's reflection";
        const String secondName = "uTint" + std::to_string(kFirst + kRelinkOffset + i);
        const GLint secondTint = GetUniformLocation(program, secondName.c_str());
        ASSERT_GE(secondTint, 0) << secondName;

        const auto& object = Object(program);
        ASSERT_NE(object, nullptr);
        object->JoinLinkAndSpirv();
        EXPECT_TRUE(object->GetSpirvStatus()) << "the relink's own phase B must have produced SPIR-V";
        EXPECT_FALSE(object->GetGeneratedSpirv().empty());

        // And the dropped buffer really was dropped: a freshly linked program's uniforms read
        // back as zero.
        GLfloat tint[3] = {-1.0f, -1.0f, -1.0f};
        GetUniformfv(program, secondTint, tint);
        for (int c = 0; c < 3; ++c) {
            EXPECT_FLOAT_EQ(tint[c], 0.0f)
                << "a uniform write buffered against the cancelled link leaked into the relink, component " << c;
        }
    }

    EXPECT_GT(relinked, 0) << "no relink ever landed inside the A->B window";
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// Destroying a program whose phase B is still queued must ABANDON it, not wait for it. The
// property is timed rather than asserted structurally, and self-calibrated: deleting a whole
// batch of programs with outstanding SPIR-V jobs has to cost a small fraction of what draining
// the same number of jobs costs. Replacing CancelLink's cooperative Cancel() with a Wait()
// would make the two times equal - which is precisely the GL-thread stall the design forbids,
// and which the previous shape of this case could not see.
TEST_F(AsyncSpirvPhaseTest, DeletingAProgramWithAPendingSpirvJobDoesNotBlock) {
    const AsyncModeScope async(true);
    const SingleWorkerScope oneWorker;
    constexpr int kPrograms = 24;
    constexpr int kHalf = kPrograms / 2;
    constexpr int kFirst = 36000;

    Vector<String> sources;
    const Vector<GLuint> programs = LinkBatch(kPrograms, kFirst, sources);

    // Settle phase A for every program without touching phase B.
    for (int i = 0; i < kPrograms; ++i) {
        ASSERT_EQ(QueryLinkStatus(programs[static_cast<SizeT>(i)]), GL_TRUE)
            << QueryProgramInfoLog(programs[static_cast<SizeT>(i)]);
    }

    int outstandingBeforeDelete = 0;
    for (int i = 0; i < kHalf; ++i) {
        const GLuint program = programs[static_cast<SizeT>(i)];
        if (!SpirvIsSettled(program)) ++outstandingBeforeDelete;
        // Buffered writes on the destruction path, so CancelLink's drop of
        // m_pendingUniformWrites/m_pendingUniformBytes is exercised rather than assumed.
        const String n = std::to_string(kFirst + i);
        const GLint locTint = GetUniformLocation(program, ("uTint" + n).c_str());
        if (locTint >= 0) {
            const GLfloat tint[3] = {1.0f, 2.0f, 3.0f};
            ProgramUniform3fv(program, locTint, 1, tint);
        }
    }

    const auto deleteStart = std::chrono::steady_clock::now();
    for (int i = 0; i < kHalf; ++i) {
        DeleteProgram(programs[static_cast<SizeT>(i)]);
    }
    const auto deleteEnd = std::chrono::steady_clock::now();

    // The calibration run: the same number of phase-B jobs, actually drained. Counted first,
    // so a machine that drained everything in the background cannot turn the bound below into
    // a comparison between two zeroes without saying so.
    int outstandingBeforeDrain = 0;
    for (int i = kHalf; i < kPrograms; ++i) {
        if (!SpirvIsSettled(programs[static_cast<SizeT>(i)])) ++outstandingBeforeDrain;
    }
    const auto drainStart = std::chrono::steady_clock::now();
    for (int i = kHalf; i < kPrograms; ++i) {
        const auto& object = Object(programs[static_cast<SizeT>(i)]);
        ASSERT_NE(object, nullptr);
        object->JoinLinkAndSpirv();
    }
    const auto drainEnd = std::chrono::steady_clock::now();

    const auto deleteUs =
        std::chrono::duration_cast<std::chrono::microseconds>(deleteEnd - deleteStart).count();
    const auto drainUs = std::chrono::duration_cast<std::chrono::microseconds>(drainEnd - drainStart).count();

    EXPECT_GT(outstandingBeforeDelete, 0) << "no program was deleted inside the A->B window";
    EXPECT_GT(outstandingBeforeDrain, 0) << "nothing was left to drain, so the timing bound has no calibration";
    if (outstandingBeforeDelete > 0 && outstandingBeforeDrain > 0 && drainUs > 2000) {
        EXPECT_LT(deleteUs, drainUs / 4)
            << "deleting " << kHalf << " programs with outstanding SPIR-V jobs took " << deleteUs
            << " us against " << drainUs << " us to drain the same number - glDeleteProgram is waiting for them";
    }
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}

// ---------------------------------------------------------------------------------------
// Inline equivalence
// ---------------------------------------------------------------------------------------

// The contract the async-off path has always had: it is byte-identical to the synchronous
// implementation. The split runs two bodies instead of one, in order, on the calling thread -
// and the artifacts it produces must equal what the asynchronous path produces.
TEST_F(AsyncSpirvPhaseTest, AsyncOffAndAsyncOnProduceIdenticalSpirvAndShadow) {
    // The ASYNC arm runs FIRST, deliberately. Both arms must compile the same source text for
    // their SPIR-V to be comparable, and the first arm to run is the one that pays for the
    // cold path: it misses the per-context ShaderPreprocessCache and therefore executes
    // PreprocessShaderSource, the reserved-identifier scan and both lexical extractions. Run
    // the sync arm first and the async arm becomes a cache hit that never runs any of that on
    // a worker - which is exactly the half this case exists to compare.
    const SingleWorkerScope oneWorker;

    Vector<Uint64> asyncDigest;
    Uint asyncUboSize = 0;
    Vector<Uint> asyncOffsets;
    Vector<Uint64> syncDigest;
    Uint syncUboSize = 0;
    Vector<Uint> syncOffsets;

    const auto buildOnce = [&](const Bool async, Vector<Uint64>& digest, Uint& uboSize, Vector<Uint>& offsets) {
        const AsyncModeScope scope(async);
        // A witness that this arm really ran in the mode it claims: without it, any ambient
        // reason for AsyncShaderCompileActive() to be false degrades the case to sync-vs-sync
        // and it still passes.
        ASSERT_EQ(MG_Util::Async::AsyncShaderCompileActive(), async)
            << "the arm did not run in the mode it was asked for";

        // Give the single worker a backlog to chew on, so "was it actually asynchronous" is a
        // deterministic observation rather than a race with a fast pool.
        Vector<String> backlogSources;
        Vector<GLuint> backlog;
        if (async) {
            backlog = LinkBatch(6, 37500, backlogSources);
        }

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

        const auto& object = Object(program);
        ASSERT_NE(object, nullptr);
        if (async) {
            // Nothing has been read yet, and the worker is busy with the backlog: the link
            // must genuinely still be outstanding.
            EXPECT_FALSE(object->IsLinkComplete()) << "the async arm settled before anything read it";
        } else {
            // The whole point of the mode: nothing is outstanding when glLinkProgram returns.
            EXPECT_TRUE(object->IsLinkComplete());
        }

        ASSERT_EQ(QueryLinkStatus(program), GL_TRUE) << QueryProgramInfoLog(program);
        digest = SpirvDigest(program);
        uboSize = object->GetUBOSize();
        for (Uint location = 0; location <= object->GetMaxUniformLocation(); ++location) {
            offsets.push_back(object->GetUniformOffset(location));
        }
        EXPECT_TRUE(object->GetSpirvStatus());

        for (const GLuint backlogProgram : backlog) {
            DeleteProgram(backlogProgram);
        }
    };

    buildOnce(true, asyncDigest, asyncUboSize, asyncOffsets);
    ASSERT_FALSE(asyncDigest.empty());

    buildOnce(false, syncDigest, syncUboSize, syncOffsets);

    EXPECT_EQ(asyncDigest, syncDigest) << "the two modes produced different SPIR-V";
    EXPECT_EQ(asyncUboSize, syncUboSize);
    EXPECT_EQ(asyncOffsets, syncOffsets);
    EXPECT_EQ(GetError(), GL_NO_ERROR);
}
