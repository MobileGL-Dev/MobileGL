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
#include <map>
#include <set>
#include <string>
#include <vector>

#include <spirv-tools/libspirv.hpp>

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

// ---------------------------------------------------------------------------------------
// The program that crashed the device, replayed end to end through the async frontend
// ---------------------------------------------------------------------------------------
//
// This is the shape that killed DirectVulkan on an Adreno 830: an Iris-transformed shader
// pair whose vertex stage declares inputs that Iris does not bind through
// glBindAttribLocation and that the shader itself never reads. Before the io-resolver fix
// such an input reached SPIR-V with no Location decoration, which is invalid
// (VUID-StandaloneSpirv-Location-04916); Adreno rejected the whole pipeline with
// VK_ERROR_UNKNOWN at the first rainy-world draw while lavapipe accepted it, so no desktop
// gate could see it.
//
// The sources are the real thing, lifted verbatim from the extracted BSL corpus: a vertex
// shader carrying the victim's condition - it DECLARES mc_Entity and mc_midTexCoord without
// reading either, and Iris binds neither - paired with the iris_FragData0 fragment shader
// that consumes its varyings. It is not the device's exact pack revision (that build is not
// in the corpus), but it is a real Iris-transformed program with the same partial-binding
// shape, driven through the same call sequence.
//
// FRONTEND ONLY, deliberately: nothing here touches a backend or a driver. The replay stops
// at the SPIR-V the frontend hands over, and validates it with the same validator whose VUID
// the driver enforces.
namespace {
    const char* kIrisWeatherVs = R"GLSL(#version 330 core
// Generated by glsl-transformer
uniform mat4 iris_ProjMat;
in vec3 iris_Position;
uniform mat4 iris_ModelViewMatInverse;
uniform mat4 iris_ProjMatInverse;
uniform mat3 iris_NormalMat;
uniform mat4 iris_LightmapTextureMatrix;
uniform mat4 iris_TextureMat;
uniform mat4 iris_ModelViewMat;
in vec4 iris_Color;
uniform vec4 iris_ColorModulator;
in ivec2 iris_UV2;
in vec2 iris_UV0;
uniform float iris_FogDensity;
uniform float iris_FogStart;
uniform float iris_FogEnd;
uniform vec4 iris_FogColor;
struct iris_FogParameters {
vec4 color;
float density;
float start;
float end;
float scale;
};
iris_FogParameters iris_Fog = iris_FogParameters(iris_FogColor, iris_FogDensity, iris_FogStart, iris_FogEnd, 1.0f / (iris_FogEnd - iris_FogStart));
vec4 iris_FrontColor;
out float iris_FogFragCoord;
const int shadowMapResolution = 2048;
const float shadowDistance = 256.0f;
const float shadowMapBias = 1.0f - 25.6f / shadowDistance;
const float sunPathRotation = -40.0f;
const float ambientOcclusionLevel = 1.0f;
out vec2 texCoord, lmCoord;
out vec3 normal;
out vec3 sunVec, upVec, eastVec;
out vec4 color;
uniform int worldTime;
uniform float frameTimeCounter;
uniform float timeAngle;
uniform vec3 cameraPosition;
uniform mat4 gbufferModelView, gbufferModelViewInverse;
uniform int frameCounter;
uniform float viewWidth, viewHeight;
in vec4 mc_Entity;
in vec4 mc_midTexCoord;
float time = frameTimeCounter * 1.0f;
uniform float framemod8;
uniform float framemod2;
vec2 jitterOffsets8[8] = vec2[8](vec2(0.125f, -0.375f), vec2(-0.125f, 0.375f), vec2(0.625f, 0.125f), vec2(0.375f, -0.625f), vec2(-0.625f, 0.625f), vec2(-0.875f, -0.125f), vec2(0.375f, -0.875f), vec2(0.875f, 0.875f));
vec2 jitterOffsets2[2] = vec2[2](vec2(1.0f, 0.0f), vec2(0.0f, 1.0f));
uniform vec3 iris_ChunkOffset;
mat4 _iris_internal_translate(vec3 offset) {
return mat4(1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, offset.x, offset.y, offset.z, 1.0f);
}
vec4 ftransform() {
return (iris_ProjMat * (iris_ModelViewMat * _iris_internal_translate(iris_ChunkOffset))) * vec4(iris_Position, 1.0f);
}
vec2 TAAJitter(vec2 coord, float w) {
vec2 offset = jitterOffsets8[int(framemod8)] * (w / vec2(viewWidth, viewHeight));
return coord + offset;
}
void main() {
iris_FogFragCoord = 0.0f;
texCoord = (iris_TextureMat * vec4(iris_UV0, 0.0f, 1.0f)).xy;
lmCoord = (iris_LightmapTextureMatrix * vec4(iris_UV2, 0.0f, 1.0f)).xy;
lmCoord = clamp((lmCoord - 0.03125f) * 1.06667f, vec2(0.0f), vec2(0.9333f, 1.0f));
normal = normalize(iris_NormalMat * vec3(0.0f, 0.0f, 1.0f));
color = (iris_Color * iris_ColorModulator);
const vec2 sunRotationData = vec2(cos(sunPathRotation * 0.01745329251994f), -sin(sunPathRotation * 0.01745329251994f));
float ang = fract(timeAngle - 0.25f);
ang = (ang + (cos(ang * 3.14159265358979f) * -0.5f + 0.5f - ang) / 3.0f) * 6.28318530717959f;
sunVec = normalize((gbufferModelView * vec4(vec3(-sin(ang), cos(ang) * sunRotationData) * 2000.0f, 1.0f)).xyz);
upVec = normalize(gbufferModelView[1].xyz);
eastVec = normalize(gbufferModelView[0].xyz);
gl_Position = ftransform();
gl_Position.xy = TAAJitter(gl_Position.xy, gl_Position.w);
}
)GLSL";

    const char* kIrisWeatherFs = R"GLSL(#version 330 core
// Generated by glsl-transformer
uniform mat4 iris_ProjMat;
uniform mat4 iris_ModelViewMatInverse;
uniform mat4 iris_ProjMatInverse;
uniform mat3 iris_NormalMat;
uniform mat4 iris_LightmapTextureMatrix;
uniform mat4 iris_TextureMat;
uniform mat4 iris_ModelViewMat;
uniform vec4 iris_ColorModulator;
uniform float iris_FogDensity;
uniform float iris_FogStart;
uniform float iris_FogEnd;
uniform vec4 iris_FogColor;
struct iris_FogParameters {
vec4 color;
float density;
float start;
float end;
float scale;
};
iris_FogParameters iris_Fog = iris_FogParameters(iris_FogColor, iris_FogDensity, iris_FogStart, iris_FogEnd, 1.0f / (iris_FogEnd - iris_FogStart));
uniform float iris_currentAlphaTest;
layout(location = 0) out vec4 iris_FragData0;
in float iris_FogFragCoord;
const int shadowMapResolution = 2048;
const float shadowDistance = 256.0f;
const float shadowMapBias = 1.0f - 25.6f / shadowDistance;
const float sunPathRotation = -40.0f;
const float ambientOcclusionLevel = 1.0f;
in vec2 texCoord, lmCoord;
in vec3 normal;
in vec3 sunVec, upVec, eastVec;
in vec4 color;
uniform int bedrockLevel;
uniform int frameCounter;
uniform int isEyeInWater;
uniform int moonPhase;
uniform int worldTime;
uniform float blindFactor, darknessFactor, nightVision;
uniform float cloudHeight;
uniform float endFlashIntensity;
uniform float far, near;
uniform float frameTimeCounter;
uniform float rainStrength;
uniform float screenBrightness;
uniform float shadowFade;
uniform float timeAngle, timeBrightness;
uniform float viewWidth, viewHeight;
uniform ivec2 eyeBrightnessSmooth;
uniform vec3 cameraPosition;
uniform vec3 relativeEyePosition;
uniform mat4 gbufferProjectionInverse;
uniform mat4 gbufferModelViewInverse;
uniform mat4 shadowProjection;
uniform mat4 shadowModelView;
uniform sampler2D gtexture;
uniform sampler2D noisetex;
uniform int heldBlockLightValue, heldBlockLightValue2;
float eBS = eyeBrightnessSmooth.y / 240.0f;
float sunVisibility = clamp(dot(sunVec, upVec) * 10.0f + 0.5f, 0.0f, 1.0f);
float moonVisibility = clamp(dot(-sunVec, upVec) * 10.0f + 0.5f, 0.0f, 1.0f);
float time = frameTimeCounter * 1.0f;
vec3 lightVec = sunVec * ((timeAngle < 0.5325f || timeAngle > 0.9675f) ? 1.0f : -1.0f);
float GetLuminance(vec3 color) {
return dot(color, vec3(0.299f, 0.587f, 0.114f));
}
vec3 blocklightColSqrt = vec3(255, 212, 160) * 0.85f / 255.0f;
vec3 blocklightCol = blocklightColSqrt * blocklightColSqrt;
vec3 lightMorning = vec3(255, 160, 80) * 1.2f / 255.0f;
vec3 lightDay = vec3(196, 220, 255) * 1.4f / 255.0f;
vec3 lightEvening = vec3(255, 160, 80) * 1.2f / 255.0f;
vec3 ambientMorning = vec3(255, 204, 144) * 0.35f / 255.0f;
vec3 ambientDay = vec3(120, 172, 255) * 0.6f / 255.0f;
vec3 ambientEvening = vec3(255, 204, 144) * 0.35f / 255.0f;
float moonPhaseMultiplier[8] = float[8](1.0f, 0.875f, 0.75f, 0.625f, 0.5f, 0.625f, 0.75f, 0.875f);
float nightMult = 0.3f * moonPhaseMultiplier[moonPhase];
vec3 lightNight = vec3(96, 192, 255) * 1.0f * nightMult / 255.0f;
vec3 ambientNight = vec3(96, 192, 255) * 0.6f * nightMult / 255.0f;
uniform float isDesert, isMesa, isCold, isSwamp, isMushroom, isSavanna, isJungle;
vec4 weatherRain = vec4(vec3(176, 224, 255) / 255.0f, 1.0f) * 1.2f;
vec4 weatherCold = vec4(vec3(216, 240, 255) / 255.0f, 1.0f) * 1.2f;
vec4 weatherDesert = vec4(vec3(255, 232, 180) / 255.0f, 1.0f) * 1.2f;
vec4 weatherBadlands = vec4(vec3(255, 216, 176) / 255.0f, 1.0f) * 1.2f;
vec4 weatherSwamp = vec4(vec3(200, 224, 160) / 255.0f, 1.0f) * 1.2f;
vec4 weatherMushroom = vec4(vec3(216, 216, 255) / 255.0f, 1.0f) * 1.2f;
vec4 weatherSavanna = vec4(vec3(224, 224, 224) / 255.0f, 1.0f) * 1.2f;
vec4 weatherJungle = vec4(vec3(176, 232, 232) / 255.0f, 1.0f) * 1.2f;
float weatherWeight = clamp(isCold + isDesert + isMesa + isSwamp + isMushroom + isSavanna + isJungle, 0.0f, 1.0f);
vec4 weatherCol = mix(weatherRain, (weatherCold * isCold + weatherDesert * isDesert + weatherBadlands * isMesa + weatherSwamp * isSwamp + weatherMushroom * isMushroom + weatherSavanna * isSavanna + weatherJungle * isJungle) / max(weatherWeight, 1.0E-4f), weatherWeight);
float mefade = 1.0f - clamp(abs(timeAngle - 0.5f) * 8.0f - 1.5f, 0.0f, 1.0f);
float dfade = 1.0f - pow(1.0f - timeBrightness, 1.5f);
vec3 lightSun = mix(mix(lightMorning, lightEvening, mefade), lightDay, dfade);
vec3 ambientSun = mix(mix(ambientMorning, ambientEvening, mefade), ambientDay, dfade);
vec3 lightColRaw = mix(lightNight, lightSun, sunVisibility);
vec3 lightColSqrt = mix(lightColRaw, dot(lightColRaw, vec3(0.299f, 0.587f, 0.114f)) * weatherCol.rgb, rainStrength);
vec3 lightCol = lightColSqrt * lightColSqrt;
vec3 ambientColRaw = mix(ambientNight, ambientSun, sunVisibility);
vec3 ambientColSqrt = mix(ambientColRaw, dot(ambientColRaw, vec3(0.299f, 0.587f, 0.114f)) * weatherCol.rgb, rainStrength);
vec3 ambientCol = ambientColSqrt * ambientColSqrt;
vec3 minLightColSqrt = vec3(128, 128, 128) * 0.5f / 255.0f;
vec3 minLightCol = minLightColSqrt * minLightColSqrt * 0.04f;
float sunSkyVisibility = clamp(dot(sunVec, upVec) * 2.0f + 0.5f, 0.0f, 1.0f);
vec3 lightSkyColRaw = mix(lightNight, lightSun, sunSkyVisibility);
vec3 lightSkyColSqrt = mix(lightSkyColRaw, dot(lightSkyColRaw, vec3(0.299f, 0.587f, 0.114f)) * weatherCol.rgb, rainStrength);
vec3 lightSkyCol = lightSkyColSqrt * lightSkyColSqrt;
vec3 skyColSqrt = vec3(96, 160, 255) * 1.0f / 255.0f;
vec3 fogColSqrt = vec3(96, 160, 255) * 1.0f / 255.0f;
vec3 skyCol = skyColSqrt * skyColSqrt;
vec3 fogCol = fogColSqrt * fogColSqrt;
vec3 ToNDC(vec3 pos) {
vec4 iProjDiag = vec4(gbufferProjectionInverse[0].x, gbufferProjectionInverse[1].y, gbufferProjectionInverse[2].zw);
vec3 p3 = pos * 2.0f - 1.0f;
vec4 viewPos = iProjDiag * p3.xyzz + gbufferProjectionInverse[3];
return viewPos.xyz / viewPos.w;
}
vec3 ToWorld(vec3 pos) {
return mat3(gbufferModelViewInverse) * pos + gbufferModelViewInverse[3].xyz;
}
vec3 ToShadow(vec3 pos) {
vec3 shadowpos = mat3(shadowModelView) * pos + shadowModelView[3].xyz;
return (vec3((shadowProjection)[0].x, (shadowProjection)[1].y, shadowProjection[2].z) * (shadowpos) + (shadowProjection)[3].xyz);
}
float fogDensity = 1.0f * mix(1.0f, (1.0f * isCold + 1.0f * (isDesert + isMesa + isSavanna) + 1.0f * (isSwamp + isMushroom + isJungle)) / max(weatherWeight, 1.0E-4f), weatherWeight);
vec3 GetFogColor(vec3 viewPos) {
vec3 nViewPos = normalize(viewPos);
float lViewPos = length(viewPos) / 64.0f;
lViewPos = 1.0f - exp(-lViewPos * lViewPos);
float VoU = clamp(dot(nViewPos, upVec), -1.0f, 1.0f);
float VoL = clamp(dot(nViewPos, sunVec), -1.0f, 1.0f);
float density = 0.4f;
float nightDensity = 1.0f;
float weatherDensity = 1.5f;
float groundDensity = 0.08f * (4.0f - 3.0f * sunSkyVisibility) * (10.0f * rainStrength * rainStrength + 1.0f);
float exposure = exp2(timeBrightness * 0.75f - 0.75f);
float nightExposure = exp2(-3.5f);
float baseGradient = exp(-(VoU * 0.5f + 0.5f) * 0.5f / density);
float groundVoU = clamp(-VoU * 0.5f + 0.5f, 0.0f, 1.0f);
float ground = 1.0f - exp(-groundDensity / groundVoU);
vec3 fog = skyCol;
fog *= baseGradient / (1.0f * 1.0f);
fog = fog / sqrt(fog * fog + 1.0f) * exposure * sunSkyVisibility * (1.0f * 1.0f);
float sunMix = pow((VoL * 0.5f + 0.5f) * clamp(1.0f - VoU, 0.0f, 1.0f), 2.0f - sunSkyVisibility) * pow(1.0f - timeBrightness * 0.6f, 3.0f);
float horizonMix = pow(1.0f - abs(VoU), 2.5f) * 0.125f;
float lightMix = (1.0f - (1.0f - sunMix) * (1.0f - horizonMix)) * lViewPos;
vec3 lightFog = pow(lightSun, vec3(4.0f - sunSkyVisibility)) * baseGradient;
lightFog = lightFog / (1.0f + lightFog * rainStrength);
fog = mix(sqrt(fog * (1.0f - lightMix)), sqrt(lightFog), lightMix);
fog *= fog;
float nightGradient = exp(-(VoU * 0.5f + 0.5f) * 0.35f / nightDensity);
vec3 nightFog = lightNight * lightNight * nightGradient * nightExposure;
fog = mix(nightFog, fog, sunSkyVisibility * sunSkyVisibility);
float rainGradient = exp(-(VoU * 0.5f + 0.5f) * 0.125f / weatherDensity);
vec3 weatherFog = weatherCol.rgb * weatherCol.rgb;
weatherFog *= GetLuminance(ambientCol / (weatherFog)) * (0.2f * sunSkyVisibility + 0.2f);
fog = mix(fog, weatherFog * rainGradient, rainStrength);
float exteriorFactor = eBS;
fog = mix(minLightCol * 0.5f, fog * exteriorFactor, exteriorFactor);
fog *= clamp((cameraPosition.y - bedrockLevel + 6.0f) / 8.0f, 0.0f, 1.0f);
return fog;
}
void NormalFog(inout vec3 color, vec3 viewPos) {
float viewLength = length(viewPos);
vec4 worldPos = gbufferModelViewInverse * vec4(viewPos, 1.0f);
worldPos.xyz /= worldPos.w;
float fogFactor = viewLength;
float fog = viewLength * fogDensity / 1024.0f;
float clearDay = sunSkyVisibility * (1.0f - rainStrength);
float exteriorFactor = eBS;
float fogDensityMult = mix(1.0f, 1.5f, rainStrength) / mix(1.0f / 4.0f, 1.0f, clearDay);
fogDensityMult = mix(1.0f, fogDensityMult * exteriorFactor, exteriorFactor);
fog *= fogDensityMult;
float fogDampen = 0.3f * rainStrength + 0.5f;
fog = min(fog, (fog - fogDampen) * 0.25f + fogDampen);
fog *= exp2(-max(worldPos.y + cameraPosition.y - 62, 0.0f) / exp2(7.0f));
fog = 1.0f - exp(-2.0f * pow(fog, 0.35f * clearDay * exteriorFactor + 1.25f));
vec3 fogColor = GetFogColor(viewPos);
color = mix(color, fogColor, fog);
}
void BlindFog(inout vec3 color, vec3 viewPos) {
float fog = length(viewPos) * max(blindFactor * 0.2f, darknessFactor * 0.075f);
fog = (1.0f - exp(-6.0f * fog * fog * fog)) * max(blindFactor, darknessFactor);
color = mix(color, vec3(0.0f), fog);
}
vec3 denseFogColor[2] = vec3[2](vec3(1.0f, 0.3f, 0.01f), vec3(0.1f, 0.16f, 0.2f));
void DenseFog(inout vec3 color, vec3 viewPos) {
float fog = length(viewPos) * 0.5f;
fog = (1.0f - exp(-4.0f * fog * fog * fog));
color = mix(color, denseFogColor[isEyeInWater - 2], fog);
}
vec2 ApplyDynamicHandlight(vec2 lightmap, vec3 worldPos) {
float heldLightValue = max(float(heldBlockLightValue), float(heldBlockLightValue2));
if (heldLightValue == 0.0f) return lightmap;
vec3 heldLightPos = worldPos + relativeEyePosition + vec3(0.0f, 0.5f, 0.0f);
float handlight = min((heldLightValue - 2.0f * length(heldLightPos)) / 15.0f, 0.9333f);
lightmap.x = log2(exp2(lightmap.x * 32.0f) + exp2(handlight * 32.0f)) / 32.0f;
return lightmap;
}
uniform sampler2DShadow shadowtex0;
uniform sampler2DShadow shadowtex1;
uniform sampler2D shadowcolor0;
vec2 shadowOffsets[9] = vec2[9](vec2(0.0f, 0.0f), vec2(0.0f, 1.0f), vec2(0.7f, 0.7f), vec2(1.0f, 0.0f), vec2(0.7f, -0.7f), vec2(0.0f, -1.0f), vec2(-0.7f, -0.7f), vec2(-1.0f, 0.0f), vec2(-0.7f, 0.7f));
float texture2DShadow(sampler2DShadow shadowtex, vec3 shadowPos) {
return vec4(texture(shadowtex, shadowPos)).x;
}
vec3 DistortShadow(vec3 shadowPos, float distortFactor) {
shadowPos.xy /= distortFactor;
shadowPos.z *= 0.2f;
shadowPos = shadowPos * 0.5f + 0.5f;
return shadowPos;
}
float InterleavedGradientNoise() {
float n = 52.9829189f * fract(0.06711056f * gl_FragCoord.x + 0.00583715f * gl_FragCoord.y);
return fract(n + frameCounter * 1.618f);
}
vec3 SampleFilteredShadow(vec3 shadowPos, float offset, float subsurface) {
float shadow0 = 0.0f;
for (int i = 0; i < 9; i++) {
vec2 shadowOffset = shadowOffsets[i] * offset;
shadow0 += texture2DShadow(shadowtex0, vec3(shadowPos.st + shadowOffset, shadowPos.z));
}
shadow0 /= 9.0f;
vec3 shadowCol = vec3(0.0f);
if (shadow0 < 0.999f) {
for (int i = 0; i < 9; i++) {
vec2 shadowOffset = shadowOffsets[i] * offset;
vec3 shadowColSample = texture(shadowcolor0, shadowPos.st + shadowOffset).rgb * texture2DShadow(shadowtex1, vec3(shadowPos.st + shadowOffset, shadowPos.z));
shadowCol += shadowColSample;
}
shadowCol /= 9.0f;
}
shadow0 *= mix(shadow0, 1.0f, subsurface);
shadowCol *= shadowCol;
return clamp(shadowCol * (1.0f - shadow0) + shadow0, vec3(0.0f), vec3(16.0f));
}
vec3 GetShadow(vec3 worldPos, vec3 normal, float NoL, float subsurface, float skylight) {
vec3 rawShadowPos = ToShadow(worldPos);
float distb = sqrt(dot(rawShadowPos.xy, rawShadowPos.xy));
float distortFactor = distb * shadowMapBias + (1.0f - shadowMapBias);
vec3 shadowPos = DistortShadow(rawShadowPos, distortFactor);
float shadowFade = clamp(100.0f - 100.0f * max(abs(rawShadowPos.x), abs(rawShadowPos.y)), 0.0f, 1.0f);
shadowFade *= clamp(skylight * 1000.0f - 1.0f, 0.0f, 1.0f);
if (shadowFade < 1.0E-5f) return vec3(1.0f);
float bias = 0.0f;
float offset = 1.0f / shadowMapResolution;
float biasFactor = sqrt(1.0f - NoL * NoL) / NoL;
float distortBias = distortFactor * shadowDistance / 256.0f;
distortBias *= 8.0f * distortBias;
float distanceBias = sqrt(dot(worldPos.xyz, worldPos.xyz)) * 0.005f;
bias = (distortBias * biasFactor + distanceBias + 0.05f) / shadowMapResolution;
if (subsurface > 0.0f) {
float blurFadeIn = clamp(distb * 20.0f, 0.0f, 1.0f);
float blurFadeOut = 1.0f - clamp(distb * 10.0f - 2.0f, 0.0f, 1.0f);
float blurMult = blurFadeIn * blurFadeOut * (1.0f - NoL);
blurMult = blurMult * 1.5f + 1.0f;
offset = 7.0E-4f * blurMult;
bias = 2.0E-4f;
}
shadowPos.z -= bias;
vec3 shadow = SampleFilteredShadow(shadowPos, offset, subsurface);
shadow = mix(vec3(1.0f), shadow, shadowFade);
return shadow;
}
void GetLighting(
inout vec3 albedo,
out vec3 shadow,
vec3 viewPos,
vec3 worldPos,
vec3 normal,
vec2 lightmap,
float smoothLighting,
float NoL,
float vanillaDiffuse,
float parallaxShadow,
float emission,
float subsurface,
float basicSubsurface
) {
float skylightSqr = lightmap.y * lightmap.y;
if (NoL > 0.0f || basicSubsurface > 0.0f) {
shadow = GetShadow(worldPos, normal, NoL, basicSubsurface, lightmap.y);
}
shadow *= parallaxShadow;
shadow = max(shadow, vec3(0.0f));
NoL = clamp(NoL * 1.01f - 0.01f, 0.0f, 1.0f);
float scattering = 0.0f;
if (basicSubsurface > 0.0f) {
float VoL = clamp(dot(normalize(viewPos.xyz), lightVec) * 0.5f + 0.5f, 0.0f, 1.0f);
scattering = pow(VoL, 16.0f) * (1.0f - rainStrength) * basicSubsurface * shadowFade;
NoL = mix(NoL, 1.0f, sqrt(basicSubsurface) * 0.7f);
NoL = mix(NoL, 1.0f, scattering);
}
vec3 fullShadow = max(shadow * NoL, vec3(0.0f));
float shadowMult = (1.0f - 0.95f * rainStrength) * shadowFade;
vec3 sceneLighting = mix(ambientCol * lightmap.y, lightCol, fullShadow * shadowMult);
sceneLighting *= skylightSqr * (1.0f + scattering * shadow);
float newLightmap = pow(lightmap.x, 10.0f) * 1.6f + lightmap.x * 0.6f;
vec3 blockLighting = blocklightCol * newLightmap * newLightmap;
vec3 minLighting = minLightCol * (1.0f - skylightSqr);
vec3 albedoNormalized = normalize(albedo.rgb + 1.0E-5f);
emission = pow(emission, max(1.0f, 1.0f));
vec3 emissiveLighting = mix(albedoNormalized, vec3(1.0f), emission * 0.5f);
emissiveLighting *= emission * 4.0f;
float lightFlatten = clamp(1.0f - pow(1.0f - emission, 128.0f), 0.0f, 1.0f);
vanillaDiffuse = mix(vanillaDiffuse, 1.0f, lightFlatten);
smoothLighting = mix(smoothLighting, 1.0f, lightFlatten);
float nightVisionLighting = nightVision * 0.25f;
float albedoBrightness = max(max(albedo.r, albedo.g), albedo.b);
albedo.rgb /= 1.0f + albedoBrightness * 0.25f * (1.0f - lightFlatten);
albedo *= max(sceneLighting + blockLighting + emissiveLighting + nightVisionLighting + minLighting, vec3(0.0f));
albedo *= vanillaDiffuse * smoothLighting * smoothLighting;
float desatAmount = 1.0f - sqrt(max(sqrt(length(fullShadow / 3.0f)) * lightmap.y, lightmap.y)) * sunVisibility * (1.0f - rainStrength * 0.7f);
desatAmount *= smoothstep(0.25f, 1.0f, (1.0f - lightmap.x) * (1.0f - lightmap.x)) * (1.0f - lightFlatten);
desatAmount = 1.0f - desatAmount;
vec3 desatNight = normalize(lightNight * lightNight + 1.0E-6f);
vec3 desatWeather = normalize(weatherCol.rgb * weatherCol.rgb + 1.0E-6f);
float desatNWMix = (1.0f - sunVisibility) * (1.0f - rainStrength);
vec3 desatColor = mix(desatWeather, desatNight, desatNWMix);
desatColor = mix(vec3(0.4f), desatColor, sqrt(lightmap.y)) * 1.7f;
vec3 desatAlbedo = mix(albedo, GetLuminance(albedo) * desatColor, 1.0f - 1.5f * 0.4f);
albedo = mix(desatAlbedo, albedo, desatAmount);
}
uniform float framemod8;
uniform float framemod2;
vec2 jitterOffsets8[8] = vec2[8](vec2(0.125f, -0.375f), vec2(-0.125f, 0.375f), vec2(0.625f, 0.125f), vec2(0.375f, -0.625f), vec2(-0.625f, 0.625f), vec2(-0.875f, -0.125f), vec2(0.375f, -0.875f), vec2(0.875f, 0.875f));
vec2 jitterOffsets2[2] = vec2[2](vec2(1.0f, 0.0f), vec2(0.0f, 1.0f));
vec2 TAAJitter(vec2 coord, float w) {
vec2 offset = jitterOffsets8[int(framemod8)] * (w / vec2(viewWidth, viewHeight));
return coord + offset;
}
void main() {
vec4 albedo = texture(gtexture, texCoord) * color;
{
vec2 lightmap = clamp(lmCoord, vec2(0.0f), vec2(1.0f));
vec3 screenPos = vec3(gl_FragCoord.xy / vec2(viewWidth, viewHeight), gl_FragCoord.z);
vec3 viewPos = ToNDC(vec3(TAAJitter(screenPos.xy, -0.5f), screenPos.z));
vec3 worldPos = ToWorld(viewPos);
lightmap = ApplyDynamicHandlight(lightmap, worldPos);
albedo.rgb = pow(albedo.rgb, vec3(2.2f));
float NoL = 1.0f;
float NoU = clamp(dot(normal, upVec), -1.0f, 1.0f);
float NoE = clamp(dot(normal, eastVec), -1.0f, 1.0f);
float vanillaDiffuse = (0.25f * NoU + 0.75f) + (0.667f - abs(NoE)) * (1.0f - abs(NoU)) * 0.15f;
vanillaDiffuse *= vanillaDiffuse;
vec3 shadow = vec3(0.0f);
GetLighting(albedo.rgb, shadow, viewPos, worldPos, normal, lightmap, 1.0f, NoL, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f);
albedo.rgb = sqrt(max(albedo.rgb, vec3(0.0f)));
}
iris_FragData0 = albedo;
if (!(iris_FragData0.a > iris_currentAlphaTest)) {
discard;
}
}
)GLSL";

    // Iris's partial pattern: it binds the vanilla attributes and leaves the pack's extras
    // (mc_Entity, mc_midTexCoord) unbound - which, with neither of them read by the shader, is
    // exactly the inactive-and-unbound case that reached SPIR-V undecorated.
    struct BoundAttribute {
        const char* name;
        GLint location;
    };
    const BoundAttribute kIrisWeatherBindings[] = {
        {"iris_Position", 0}, {"iris_Color", 1}, {"iris_UV0", 2}, {"iris_UV2", 3},
    };

    // Drives Iris's own call sequence and returns the linked program name.
    GLuint ReplayIrisWeatherProgram() {
        const GLuint vs = CreateShader(GL_VERTEX_SHADER);
        ShaderSource(vs, 1, &kIrisWeatherVs, nullptr);
        CompileShader(vs);
        const GLuint fs = CreateShader(GL_FRAGMENT_SHADER);
        ShaderSource(fs, 1, &kIrisWeatherFs, nullptr);
        CompileShader(fs);

        // Iris reads the log and the status for every shader, in that order.
        for (const GLuint shader : {vs, fs}) {
            GLint logLength = 0;
            GetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLength);
            if (logLength > 0) {
                std::vector<GLchar> log(static_cast<size_t>(logLength));
                GLsizei written = 0;
                GetShaderInfoLog(shader, logLength, &written, log.data());
            }
            GLint compileStatus = GL_FALSE;
            GetShaderiv(shader, GL_COMPILE_STATUS, &compileStatus);
            EXPECT_EQ(compileStatus, GL_TRUE) << "shader " << shader << " failed to compile";
        }

        const GLuint program = CreateProgram();
        AttachShader(program, vs);
        AttachShader(program, fs);
        for (const BoundAttribute& binding : kIrisWeatherBindings) {
            BindAttribLocation(program, static_cast<GLuint>(binding.location), binding.name);
        }
        LinkProgram(program);
        return program;
    }
} // namespace

TEST_F(AsyncSpirvPhaseTest, IrisWeatherProgramReplaysCleanlyThroughBothPhases) {
    for (const Bool optimisticQuirk : {false, true}) {
        const AsyncModeScope async(true);
        const MG_Config::QuirkOverride savedQuirk = MG_Config::Features.AsyncOptimisticShaderStatus;
        MG_Config::Features.AsyncOptimisticShaderStatus =
            optimisticQuirk ? MG_Config::QuirkOverride::ForceOn : MG_Config::QuirkOverride::ForceOff;

        const GLuint program = ReplayIrisWeatherProgram();

        // ---- phase A: LINK_STATUS is the join, and it must be truthful ----
        GLint linkStatus = GL_FALSE;
        GetProgramiv(program, GL_LINK_STATUS, &linkStatus);
        ASSERT_EQ(linkStatus, GL_TRUE) << QueryProgramInfoLog(program)
                                       << " (optimistic quirk " << (optimisticQuirk ? "on" : "off") << ")";

        // Iris queries uniforms straight after the status read; these are phase-A answers.
        EXPECT_GE(GetUniformLocation(program, "iris_ModelViewMatrix"), -1);
        EXPECT_GE(GetUniformLocation(program, "iris_ProjectionMatrix"), -1);
        EXPECT_GE(GetUniformLocation(program, "texture"), -1);

        // ---- the API bindings survived exactly ----
        for (const BoundAttribute& binding : kIrisWeatherBindings) {
            EXPECT_EQ(GetAttribLocation(program, binding.name), binding.location) << binding.name;
        }

        // ---- settle phase B through a gated getter and check what it published ----
        const auto& object = Object(program);
        ASSERT_NE(object, nullptr);
        const auto& modules = object->GetGeneratedSpirv(); // joins phase B
        EXPECT_TRUE(object->IsSpirvComplete());
        EXPECT_TRUE(object->GetSpirvStatus());
        ASSERT_FALSE(modules.empty());
        EXPECT_GT(object->GetUBOSize(), 0u) << "the uniform shadow should have been allocated";
        EXPECT_NE(object->GetUniformOffset(0), MG_State::GLState::ProgramObject::kInvalidUniformOffset);

        // ---- every module the frontend hands over must be valid SPIR-V ----
        // NOTE ON WHAT THIS DOES AND DOES NOT CATCH. It validates the modules AFTER spirv-opt,
        // which is what a backend actually receives - but AggressiveDCE deletes an input that
        // nothing reads, so for a merely-unused attribute this assertion cannot fail even with
        // the io-resolver bug reinstated (verified by mutation). The discriminating gate for
        // VUID-StandaloneSpirv-Location-04916 is
        // ProgramUtilTest.PartiallyBoundVertexInputsAllReceiveALocation, which validates the
        // RAW GlslangToSpv output before the optimizer can hide the defect. This assertion is
        // still worth having: it is the end-to-end guarantee that whatever the frontend ships
        // to a driver is valid, and it would catch a regression whose variable SURVIVES DCE -
        // which is precisely what the device victim did.
        spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_1);
        String validatorMessages;
        tools.SetMessageConsumer([&validatorMessages](spv_message_level_t, const char*, const spv_position_t&,
                                                     const char* message) {
            if (message != nullptr) validatorMessages += String(message) + "\n";
        });
        for (SizeT i = 0; i < modules.size(); ++i) {
            validatorMessages.clear();
            EXPECT_TRUE(tools.Validate(modules[i]))
                << "module " << i << " is not valid SPIR-V - Adreno rejects the pipeline for this while "
                << "lavapipe tolerates it:\n" << validatorMessages;
        }

        // ---- every vertex input carries a unique Location ----
        const Int vsIndex = object->GetShaderIndexByStage(ShaderStage::Vertex);
        ASSERT_GE(vsIndex, 0);
        const auto& vsModule = modules[static_cast<SizeT>(vsIndex)];
        constexpr unsigned kOpDecorate = 71, kOpVariable = 59;
        constexpr unsigned kDecorationBuiltIn = 11, kDecorationLocation = 30;
        constexpr unsigned kStorageClassInput = 1;
        std::map<unsigned, unsigned> locationById;
        std::set<unsigned> builtInIds;
        std::vector<unsigned> inputIds;
        for (SizeT i = 5; i < vsModule.size();) {
            const unsigned wordCount = vsModule[i] >> 16;
            const unsigned opcode = vsModule[i] & 0xFFFFu;
            ASSERT_GT(wordCount, 0u);
            if (i + wordCount > vsModule.size()) break;
            if (opcode == kOpDecorate && wordCount >= 4 && vsModule[i + 2] == kDecorationLocation) {
                locationById[vsModule[i + 1]] = vsModule[i + 3];
            } else if (opcode == kOpDecorate && wordCount >= 3 && vsModule[i + 2] == kDecorationBuiltIn) {
                builtInIds.insert(vsModule[i + 1]);
            } else if (opcode == kOpVariable && wordCount >= 4 && vsModule[i + 3] == kStorageClassInput) {
                inputIds.push_back(vsModule[i + 2]);
            }
            i += wordCount;
        }
        std::set<unsigned> usedLocations;
        SizeT checkedInputs = 0;
        for (const unsigned id : inputIds) {
            if (builtInIds.count(id) != 0) continue;
            const auto it = locationById.find(id);
            ASSERT_NE(it, locationById.end())
                << "a vertex input reached SPIR-V with no Location decoration (optimistic quirk "
                << (optimisticQuirk ? "on" : "off") << ")";
            EXPECT_TRUE(usedLocations.insert(it->second).second)
                << "two vertex inputs share location " << it->second;
            ++checkedInputs;
        }
        EXPECT_GT(checkedInputs, 0u) << "no vertex inputs found; the scan proved nothing";

        EXPECT_EQ(GetError(), GL_NO_ERROR);
        MG_Config::Features.AsyncOptimisticShaderStatus = savedQuirk;
    }
}
