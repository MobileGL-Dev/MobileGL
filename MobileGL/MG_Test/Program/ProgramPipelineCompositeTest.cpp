// MobileGL - MobileGL/MG_Test/Program/ProgramPipelineCompositeTest.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// The hidden composite program a pipeline draw goes through (MG_State/GLState/Core.cpp,
// GetProgramForDraw), interrogated directly rather than through pixels.
//
// Two properties live here that the integration scenarios cannot see, because both are about
// the composite as an OBJECT rather than about what it paints:
//
//   1. WHICH stage's uniform value ends up in its single slot when several stages declare the
//      same name. The rendering cases pin the answer for the shapes an application actually
//      writes; these pin the rule itself, including the tie.
//   2. WHETHER it is the same object from one draw to the next. A composite rebuild is a full
//      synchronous Link() plus a new program identity that empties both backends' per-program
//      registries, and nothing about the resulting IMAGE would change if it happened on every
//      draw - so an assertion on pixels can never catch that regression.

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "Config.h"
#include "Includes.h"
#include "Init.h"
#include "MG_Impl/GLImpl/Getter/GL_Getter.h"
#include "MG_Impl/GLImpl/Program/GL_Program.h"
#include "MG_Impl/GLImpl/Program/GL_ProgramPipeline.h"
#include "MG_State/GLState/Core.h"

using namespace MobileGL;
using namespace MobileGL::MG_Impl::GLImpl;

namespace {

    // Both stages declare `u_shared`, which is the shared-header idiom (one header included by
    // every stage) and the shape that used to render nothing: the fragment stage's untouched
    // zero default overwrote the vertex stage's written value on the way into the composite.
    const char* kSharedUniformVs = R"(#version 430 core
out gl_PerVertex { vec4 gl_Position; };
uniform vec4 u_shared;
uniform vec4 u_vsOnly;
void main() { gl_Position = u_shared + u_vsOnly; }
)";

    const char* kSharedUniformFs = R"(#version 430 core
uniform vec4 u_shared;
out vec4 o_color;
void main() { o_color = u_shared; }
)";

    const char* kArrayUniformVs = R"(#version 430 core
out gl_PerVertex { vec4 gl_Position; };
uniform vec4 u_arr[4];
void main() { gl_Position = u_arr[0] + u_arr[1] + u_arr[2] + u_arr[3]; }
)";

    const char* kArrayUniformFs = R"(#version 430 core
uniform vec4 u_arr[4];
out vec4 o_color;
void main() { o_color = u_arr[0] + u_arr[1] + u_arr[2] + u_arr[3]; }
)";

    const char* kSamplerVs = R"(#version 430 core
out gl_PerVertex { vec4 gl_Position; };
void main() { gl_Position = vec4(0.0, 0.0, 0.0, 1.0); }
)";

    const char* kSamplerFs = R"(#version 430 core
uniform sampler2D u_tex;
out vec4 o_color;
void main() { o_color = texture(u_tex, vec2(0.0)); }
)";

    class ProgramPipelineCompositeTest : public ::testing::Test {
    protected:
        void SetUp() override { MobileGL::Initialize(); }

        // Built by hand rather than through glCreateShaderProgramv, for the reason AsyncLinkTest
        // gives: that entry point detaches the shader right after linking, so a relink would
        // leave the stage program with nothing to composite from - and one of the cases below
        // relinks on purpose.
        GLuint MakeSeparableProgram(const GLenum stage, const char* source) {
            const GLuint shader = CreateShader(stage);
            ShaderSource(shader, 1, &source, nullptr);
            CompileShader(shader);
            const GLuint program = CreateProgram();
            ProgramParameteri(program, GL_PROGRAM_SEPARABLE, GL_TRUE);
            AttachShader(program, shader);
            LinkProgram(program);
            GLint linked = GL_FALSE;
            GetProgramiv(program, GL_LINK_STATUS, &linked);
            EXPECT_EQ(linked, GL_TRUE) << "separable stage program did not link";
            return program;
        }

        // The composite the next draw would run, settled.
        static SharedPtr<MG_State::GLState::ProgramObject> DrawProgram() {
            return MG_State::pGLContext->GetProgramForDraw();
        }

        // A uniform's value read out of a program's own shadow, by name. This is what the draw
        // would upload, which is the thing under test - glGetUniform* would answer the same for
        // the STAGE programs but has no way to name the composite at all.
        static std::vector<float> ReadVec4(MG_State::GLState::ProgramObject& program, const String& name) {
            const Int location = program.GetUniformLocation(name);
            if (location < 0) return {};
            const Uint offset = program.GetUniformOffset(static_cast<Uint>(location));
            const auto* ubo = static_cast<const char*>(program.GetUBOData());
            if (ubo == nullptr || offset == MG_State::GLState::ProgramObject::kInvalidUniformOffset ||
                offset + 4 * sizeof(float) > program.GetUBOSize()) {
                return {};
            }
            std::vector<float> value(4);
            std::memcpy(value.data(), ubo + offset, 4 * sizeof(float));
            return value;
        }
    };

} // namespace

// ---------------------------------------------------------------------------------------
// Which stage wins the composite's single slot
// ---------------------------------------------------------------------------------------

// THE defect. Both stages declare `u_shared`; only the VERTEX program is ever written to.
// Walking the stages in order and copying every active uniform unconditionally meant the
// fragment stage's untouched zero default landed last and won, so the composite drew zeros - a
// whole frame of nothing, from a program that had been set up entirely correctly.
TEST_F(ProgramPipelineCompositeTest, AWrittenStageValueIsNotClobberedByAnotherStagesUntouchedDeclaration) {
    const GLuint vs = MakeSeparableProgram(GL_VERTEX_SHADER, kSharedUniformVs);
    const GLuint fs = MakeSeparableProgram(GL_FRAGMENT_SHADER, kSharedUniformFs);

    GLuint pipeline = 0;
    GenProgramPipelines(1, &pipeline);
    BindProgramPipeline(pipeline);
    UseProgramStages(pipeline, GL_VERTEX_SHADER_BIT, vs);
    UseProgramStages(pipeline, GL_FRAGMENT_SHADER_BIT, fs);
    ASSERT_EQ(GetError(), GL_NO_ERROR);

    // Exactly what an application does: point glUniform* at the vertex stage and write there.
    // The fragment program is never written to and holds nothing but GL's zero default.
    ActiveShaderProgram(pipeline, vs);
    const GLint location = GetUniformLocation(vs, "u_shared");
    ASSERT_GE(location, 0);
    const float written[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    Uniform4fv(location, 1, written);
    ASSERT_EQ(GetError(), GL_NO_ERROR);

    const auto composite = DrawProgram();
    ASSERT_NE(composite, nullptr);
    const std::vector<float> value = ReadVec4(*composite, "u_shared");
    ASSERT_EQ(value.size(), 4u) << "u_shared has no backing storage in the composite";
    EXPECT_EQ(value, (std::vector<float>{0.25f, 0.5f, 0.75f, 1.0f}))
        << "the fragment stage's untouched declaration overwrote the vertex stage's written value";

    // The uniform only one stage declares is unaffected either way; it is here so a mirror that
    // copied nothing at all would not pass this case by accident.
    ActiveShaderProgram(pipeline, vs);
    const GLint vsOnly = GetUniformLocation(vs, "u_vsOnly");
    ASSERT_GE(vsOnly, 0);
    const float other[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    Uniform4fv(vsOnly, 1, other);
    const auto refreshed = DrawProgram();
    EXPECT_EQ(ReadVec4(*refreshed, "u_vsOnly"), (std::vector<float>{1.0f, 2.0f, 3.0f, 4.0f}));
    EXPECT_EQ(GetError(), GL_NO_ERROR);

    BindProgramPipeline(0);
    DeleteProgramPipelines(1, &pipeline);
}

// The tie the fix cannot make disappear: BOTH stages were written, and the composite still has
// one slot. The documented rule is last WRITTEN-TO graphics stage wins, in ShaderStage enum
// order - deterministic, and reachable only by a stage holding a real application value.
TEST_F(ProgramPipelineCompositeTest, WhenBothStagesWereWrittenTheLastGraphicsStageWins) {
    const GLuint vs = MakeSeparableProgram(GL_VERTEX_SHADER, kSharedUniformVs);
    const GLuint fs = MakeSeparableProgram(GL_FRAGMENT_SHADER, kSharedUniformFs);

    GLuint pipeline = 0;
    GenProgramPipelines(1, &pipeline);
    BindProgramPipeline(pipeline);
    UseProgramStages(pipeline, GL_VERTEX_SHADER_BIT, vs);
    UseProgramStages(pipeline, GL_FRAGMENT_SHADER_BIT, fs);

    const float fromVs[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    const float fromFs[4] = {2.0f, 2.0f, 2.0f, 2.0f};
    // Written in the order VS then FS...
    ProgramUniform4fv(vs, GetUniformLocation(vs, "u_shared"), 1, fromVs);
    ProgramUniform4fv(fs, GetUniformLocation(fs, "u_shared"), 1, fromFs);
    ASSERT_EQ(GetError(), GL_NO_ERROR);
    EXPECT_EQ(ReadVec4(*DrawProgram(), "u_shared"), (std::vector<float>{2.0f, 2.0f, 2.0f, 2.0f}));

    // ...and in the order FS then VS. The answer is the same, because the rule is stage order
    // and not write order - which is the honest statement of what the dirty set can support.
    ProgramUniform4fv(fs, GetUniformLocation(fs, "u_shared"), 1, fromFs);
    ProgramUniform4fv(vs, GetUniformLocation(vs, "u_shared"), 1, fromVs);
    ASSERT_EQ(GetError(), GL_NO_ERROR);
    EXPECT_EQ(ReadVec4(*DrawProgram(), "u_shared"), (std::vector<float>{2.0f, 2.0f, 2.0f, 2.0f}))
        << "the both-written tie must be decided by stage order, deterministically";

    BindProgramPipeline(0);
    DeleteProgramPipelines(1, &pipeline);
}

// glProgramUniform* addresses a program by NAME and needs neither a current program nor an
// active shader program, so it is a write path that never touches the pipeline at all. It has
// to record the write exactly like glUniform* does.
TEST_F(ProgramPipelineCompositeTest, ProgramUniformOnAnUnboundStageProgramReachesTheComposite) {
    const GLuint vs = MakeSeparableProgram(GL_VERTEX_SHADER, kSharedUniformVs);
    const GLuint fs = MakeSeparableProgram(GL_FRAGMENT_SHADER, kSharedUniformFs);

    GLuint pipeline = 0;
    GenProgramPipelines(1, &pipeline);
    UseProgramStages(pipeline, GL_VERTEX_SHADER_BIT, vs);
    UseProgramStages(pipeline, GL_FRAGMENT_SHADER_BIT, fs);

    // Deliberately BEFORE the bind, and with no glActiveShaderProgram anywhere: the write has
    // to survive from here to a draw that has not been set up yet.
    const float written[4] = {9.0f, 8.0f, 7.0f, 6.0f};
    ProgramUniform4fv(vs, GetUniformLocation(vs, "u_vsOnly"), 1, written);
    ASSERT_EQ(GetError(), GL_NO_ERROR);

    BindProgramPipeline(pipeline);
    EXPECT_EQ(ReadVec4(*DrawProgram(), "u_vsOnly"), (std::vector<float>{9.0f, 8.0f, 7.0f, 6.0f}));
    EXPECT_EQ(GetError(), GL_NO_ERROR);

    BindProgramPipeline(0);
    DeleteProgramPipelines(1, &pipeline);
}

// Array uniforms are written at ELEMENT locations, so the record has to be per location and not
// per name: a stage that wrote `u_arr[2]` and nothing else must carry element 2 across and
// leave the rest to whichever stage owns them.
TEST_F(ProgramPipelineCompositeTest, ArrayElementWritesMirrorPerElement) {
    const GLuint vs = MakeSeparableProgram(GL_VERTEX_SHADER, kArrayUniformVs);
    const GLuint fs = MakeSeparableProgram(GL_FRAGMENT_SHADER, kArrayUniformFs);

    GLuint pipeline = 0;
    GenProgramPipelines(1, &pipeline);
    BindProgramPipeline(pipeline);
    UseProgramStages(pipeline, GL_VERTEX_SHADER_BIT, vs);
    UseProgramStages(pipeline, GL_FRAGMENT_SHADER_BIT, fs);

    // Non-prefix on purpose: elements 1 and 3 from the vertex stage, element 2 from the fragment
    // stage, element 0 from nobody. A per-name record would have carried whole arrays and let
    // one stage's zeros take the other's elements.
    const float one[4] = {11.0f, 11.0f, 11.0f, 11.0f};
    const float three[4] = {33.0f, 33.0f, 33.0f, 33.0f};
    const float two[4] = {22.0f, 22.0f, 22.0f, 22.0f};
    ProgramUniform4fv(vs, GetUniformLocation(vs, "u_arr[1]"), 1, one);
    ProgramUniform4fv(vs, GetUniformLocation(vs, "u_arr[3]"), 1, three);
    ProgramUniform4fv(fs, GetUniformLocation(fs, "u_arr[2]"), 1, two);
    ASSERT_EQ(GetError(), GL_NO_ERROR);

    const auto composite = DrawProgram();
    ASSERT_NE(composite, nullptr);
    EXPECT_EQ(ReadVec4(*composite, "u_arr[0]"), (std::vector<float>{0.0f, 0.0f, 0.0f, 0.0f}));
    EXPECT_EQ(ReadVec4(*composite, "u_arr[1]"), (std::vector<float>{11.0f, 11.0f, 11.0f, 11.0f}));
    EXPECT_EQ(ReadVec4(*composite, "u_arr[2]"), (std::vector<float>{22.0f, 22.0f, 22.0f, 22.0f}));
    EXPECT_EQ(ReadVec4(*composite, "u_arr[3]"), (std::vector<float>{33.0f, 33.0f, 33.0f, 33.0f}));
    EXPECT_EQ(GetError(), GL_NO_ERROR);

    // A multi-element glUniform*v run marks each location it actually reaches.
    const float tail[8] = {44.0f, 44.0f, 44.0f, 44.0f, 55.0f, 55.0f, 55.0f, 55.0f};
    ActiveShaderProgram(pipeline, fs);
    Uniform4fv(GetUniformLocation(fs, "u_arr[2]"), 2, tail);
    ASSERT_EQ(GetError(), GL_NO_ERROR);
    const auto refreshed = DrawProgram();
    EXPECT_EQ(ReadVec4(*refreshed, "u_arr[2]"), (std::vector<float>{44.0f, 44.0f, 44.0f, 44.0f}));
    EXPECT_EQ(ReadVec4(*refreshed, "u_arr[3]"), (std::vector<float>{55.0f, 55.0f, 55.0f, 55.0f}))
        << "the second element of a count=2 write was never recorded";

    BindProgramPipeline(0);
    DeleteProgramPipelines(1, &pipeline);
}

// Relinking resets a program's uniforms to their initial values (GL 4.6 core 7.6), so the record
// of what was written has to be reset with them. If it survived, the composite built after the
// relink would be handed values the stage program no longer holds.
TEST_F(ProgramPipelineCompositeTest, RelinkingAStageProgramClearsWhatItHadWritten) {
    const GLuint vs = MakeSeparableProgram(GL_VERTEX_SHADER, kSharedUniformVs);
    const GLuint fs = MakeSeparableProgram(GL_FRAGMENT_SHADER, kSharedUniformFs);

    GLuint pipeline = 0;
    GenProgramPipelines(1, &pipeline);
    BindProgramPipeline(pipeline);
    UseProgramStages(pipeline, GL_VERTEX_SHADER_BIT, vs);
    UseProgramStages(pipeline, GL_FRAGMENT_SHADER_BIT, fs);

    const float written[4] = {5.0f, 6.0f, 7.0f, 8.0f};
    ProgramUniform4fv(vs, GetUniformLocation(vs, "u_vsOnly"), 1, written);
    ASSERT_EQ(ReadVec4(*DrawProgram(), "u_vsOnly"), (std::vector<float>{5.0f, 6.0f, 7.0f, 8.0f}));

    LinkProgram(vs);
    GLint linked = GL_FALSE;
    GetProgramiv(vs, GL_LINK_STATUS, &linked);
    ASSERT_EQ(linked, GL_TRUE);

    const auto composite = DrawProgram();
    ASSERT_NE(composite, nullptr);
    EXPECT_EQ(ReadVec4(*composite, "u_vsOnly"), (std::vector<float>{0.0f, 0.0f, 0.0f, 0.0f}))
        << "a relinked stage program carried its pre-relink value into the new composite";
    EXPECT_EQ(GetError(), GL_NO_ERROR);

    // ...and writing again after the relink is recorded afresh.
    const float rewritten[4] = {1.5f, 2.5f, 3.5f, 4.5f};
    ProgramUniform4fv(vs, GetUniformLocation(vs, "u_vsOnly"), 1, rewritten);
    EXPECT_EQ(ReadVec4(*DrawProgram(), "u_vsOnly"), (std::vector<float>{1.5f, 2.5f, 3.5f, 4.5f}));
    EXPECT_EQ(GetError(), GL_NO_ERROR);

    BindProgramPipeline(0);
    DeleteProgramPipelines(1, &pipeline);
}

// ---------------------------------------------------------------------------------------
// Composite cache stability
// ---------------------------------------------------------------------------------------

// The SSO-conformance shape, and the reason the composite cache stopped being keyed on the
// backend state version: pick a stage program, then per draw set a sampler unit and draw.
// glUniform1i on a sampler bumps that version, so the signature changed on every iteration and
// every single draw threw the composite away and relinked it - glslang, SPIR-V and spirv-opt,
// synchronously, inside the draw - handing the backends a brand-new program identity each time.
//
// Asserted on the composite POINTER, which is the honest observable: it is the object both
// backends key their per-program registries and pipeline memos on, so "same pointer" is exactly
// the property that was lost.
TEST_F(ProgramPipelineCompositeTest, ASamplerWritePerDrawDoesNotRebuildTheComposite) {
    const GLuint vs = MakeSeparableProgram(GL_VERTEX_SHADER, kSamplerVs);
    const GLuint fs = MakeSeparableProgram(GL_FRAGMENT_SHADER, kSamplerFs);

    GLuint pipeline = 0;
    GenProgramPipelines(1, &pipeline);
    BindProgramPipeline(pipeline);
    UseProgramStages(pipeline, GL_VERTEX_SHADER_BIT, vs);
    UseProgramStages(pipeline, GL_FRAGMENT_SHADER_BIT, fs);
    ActiveShaderProgram(pipeline, fs);
    ASSERT_EQ(GetError(), GL_NO_ERROR);

    const GLint sampler = GetUniformLocation(fs, "u_tex");
    ASSERT_GE(sampler, 0);

    const auto first = DrawProgram();
    ASSERT_NE(first, nullptr);
    const Uint64 firstLifetime = first->GetLifetimeId();
    const Int compositeSampler = first->GetUniformLocation("u_tex");
    ASSERT_GE(compositeSampler, 0);

    for (GLint unit = 0; unit < 8; ++unit) {
        Uniform1i(sampler, unit);
        const auto composite = DrawProgram();
        ASSERT_NE(composite, nullptr);
        EXPECT_EQ(composite.get(), first.get())
            << "the composite was rebuilt by a sampler-unit write at unit " << unit;
        EXPECT_EQ(composite->GetLifetimeId(), firstLifetime) << "the composite's identity changed at unit " << unit;
        // The value still has to ARRIVE - the whole point is that the mirror carries it now that
        // the rebuild no longer does.
        EXPECT_EQ(composite->GetUniformSamplerOrImageUnitIndex(static_cast<Uint>(compositeSampler)), unit)
            << "the sampler unit did not reach the composite at unit " << unit;
    }
    EXPECT_EQ(GetError(), GL_NO_ERROR);

    // A relink, by contrast, MUST replace it: that is the one thing the signature still tracks.
    LinkProgram(fs);
    GLint linked = GL_FALSE;
    GetProgramiv(fs, GL_LINK_STATUS, &linked);
    ASSERT_EQ(linked, GL_TRUE);
    const auto afterRelink = DrawProgram();
    ASSERT_NE(afterRelink, nullptr);
    EXPECT_NE(afterRelink.get(), first.get()) << "a relinked stage program must rebuild the composite";

    BindProgramPipeline(0);
    DeleteProgramPipelines(1, &pipeline);
}

// The monolithic path must be untouched by any of this: a plain glUseProgram program is not
// separable, records nothing, and is its own draw program.
TEST_F(ProgramPipelineCompositeTest, AMonolithicProgramRecordsNothingAndIsItsOwnDrawProgram) {
    const char* vsSource = R"(#version 430 core
uniform vec4 u_shared;
void main() { gl_Position = u_shared; }
)";
    const char* fsSource = R"(#version 430 core
uniform vec4 u_shared;
out vec4 o_color;
void main() { o_color = u_shared; }
)";
    const GLuint vsShader = CreateShader(GL_VERTEX_SHADER);
    ShaderSource(vsShader, 1, &vsSource, nullptr);
    CompileShader(vsShader);
    const GLuint fsShader = CreateShader(GL_FRAGMENT_SHADER);
    ShaderSource(fsShader, 1, &fsSource, nullptr);
    CompileShader(fsShader);

    const GLuint program = CreateProgram();
    AttachShader(program, vsShader);
    AttachShader(program, fsShader);
    LinkProgram(program);
    GLint linked = GL_FALSE;
    GetProgramiv(program, GL_LINK_STATUS, &linked);
    ASSERT_EQ(linked, GL_TRUE);

    UseProgram(program);
    const float written[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    Uniform4fv(GetUniformLocation(program, "u_shared"), 1, written);
    ASSERT_EQ(GetError(), GL_NO_ERROR);

    const auto drawProgram = DrawProgram();
    ASSERT_NE(drawProgram, nullptr);
    EXPECT_EQ(drawProgram->GetExternalIndex(), program) << "a current program IS the draw program";
    // Nothing was recorded, because nothing ever asked this program to be separable - which is
    // what keeps the hot uniform path free of the bookkeeping.
    EXPECT_FALSE(drawProgram->TracksUniformWrites());
    EXPECT_TRUE(drawProgram->GetWrittenUniformIndices().empty());
    EXPECT_EQ(ReadVec4(*drawProgram, "u_shared"), (std::vector<float>{1.0f, 2.0f, 3.0f, 4.0f}));

    UseProgram(0);
}
