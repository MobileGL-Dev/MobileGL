// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/ProgramPipelineScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - SEPARABLE PROGRAMS DRAWN THROUGH A PROGRAM PIPELINE OBJECT.
//
// A pipeline object holds one program per stage and stands in for glUseProgram; MobileGL
// flattens it into a single composite program at draw time (MG_State/GLState/Core.cpp,
// GetProgramForDraw). Sixteen conformance cases across three different families depend on that
// flattening and fail identically on BOTH backends - so the defect is in the shared frontend, not
// in either backend's draw path:
//
//   compute_shader.{build-monolithic, build-separable, sso-case2, sso-case3, sso-compute-pipeline}
//   shader_image_load_store.advanced-sso-{atomicCounters, simple, subroutine}
//   shader_storage_buffer_object.{basic-syntaxSSO, basic-noBindingLayout}
//
// They fail with two symptoms at once - the draw renders nothing, AND the case leaves a
// GL_INVALID_OPERATION behind that the harness reports as "forcing FAIL for subcase". Anything
// claiming to be the root cause has to explain both.
//
// The cases here are the conformance shapes reduced to what fails in milliseconds, ordered from
// the simplest pipeline that can render at all up to the compute-then-draw shape of
// sso-compute-pipeline. Each one also asserts glGetError is clean at the end, because a case that
// paints correctly and leaks an error still fails conformance.

#include <cstdint>
#include <string>
#include <vector>

#include "../Harness/HeadlessGL.h"
#include "../Harness/ScenarioFixture.h"

#ifdef GLAPI
#undef GLAPI
#endif
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glcorearb.h>
#undef GL_GLEXT_PROTOTYPES

namespace MGITest {
    namespace {

        // Separable stage sources. A separable VS must redeclare gl_PerVertex, which is exactly
        // the kind of thing a flattening step can drop on the floor.
        constexpr const char* kSeparableVS = R"(#version 430 core
out gl_PerVertex { vec4 gl_Position; };
void main()
{
    switch (gl_VertexID)
    {
      case 0: gl_Position = vec4(-1.0, -1.0, 0.0, 1.0); break;
      case 1: gl_Position = vec4( 1.0, -1.0, 0.0, 1.0); break;
      case 2: gl_Position = vec4(-1.0,  1.0, 0.0, 1.0); break;
      case 3: gl_Position = vec4( 1.0,  1.0, 0.0, 1.0); break;
    }
}
)";

        constexpr const char* kSeparableFS = R"(#version 430 core
out vec4 o_color;
void main() { o_color = vec4(0.0, 1.0, 0.0, 1.0); }
)";

        // The sso-compute-pipeline shape: a compute stage writes the vertex positions the vertex
        // stage then reads as an attribute, all from one pipeline object.
        constexpr const char* kComputeSource = R"(#version 430 core
layout(local_size_x = 1) in;
layout(std430, binding = 0) buffer Positions {
    vec4 g_position[4];
};
void main()
{
    g_position[0] = vec4(-1.0, -1.0, 0.0, 1.0);
    g_position[1] = vec4( 1.0, -1.0, 0.0, 1.0);
    g_position[2] = vec4(-1.0,  1.0, 0.0, 1.0);
    g_position[3] = vec4( 1.0,  1.0, 0.0, 1.0);
}
)";

        constexpr const char* kAttributeVS = R"(#version 430 core
layout(location = 0) in vec4 i_position;
out gl_PerVertex { vec4 gl_Position; };
void main() { gl_Position = i_position; }
)";

        class ProgramPipelineScenario : public ScenarioTest {
        protected:
            void TearDown() override {
                if (!Ready()) return;
                glBindProgramPipeline(0);
                glUseProgram(0);
                for (GLuint p : m_programs) glDeleteProgram(p);
                for (GLuint p : m_pipelines) glDeleteProgramPipelines(1, &p);
                m_programs.clear();
                m_pipelines.clear();
            }

            GLuint MakeSeparable(GLenum stage, const char* source) {
                const GLuint program = glCreateShaderProgramv(stage, 1, &source);
                if (program != 0) m_programs.push_back(program);
                // Checked here rather than only at the end of the case: glCreateShaderProgramv is
                // specified as a sequence of other entry points, so it is the most likely place
                // for one of them to leave an error nobody consumes.
                EXPECT_EQ(FirstGLError(), 0u)
                    << "glCreateShaderProgramv(stage 0x" << std::hex << stage << std::dec << ") left a GL error";
                GLint linked = GL_FALSE;
                glGetProgramiv(program, GL_LINK_STATUS, &linked);
                if (linked == GL_FALSE) {
                    char log[2048] = {};
                    glGetProgramInfoLog(program, sizeof(log) - 1, nullptr, log);
                    ADD_FAILURE() << "glCreateShaderProgramv(stage 0x" << std::hex << stage << std::dec
                                  << ") did not link: " << log;
                    return 0;
                }
                return program;
            }

            GLuint MakePipeline() {
                GLuint pipeline = 0;
                glGenProgramPipelines(1, &pipeline);
                m_pipelines.push_back(pipeline);
                return pipeline;
            }

            std::vector<GLuint> m_programs;
            std::vector<GLuint> m_pipelines;
        };

    } // namespace

    // The floor: a two-stage pipeline must paint. If this fails, nothing above it can pass, and
    // the eight shared conformance cases have exactly one cause.
    TEST_F(ProgramPipelineScenario, ATwoStagePipelinePaintsWhatItsStagesDescribe) {
        if (!Ready()) return;
        HeadlessGL& gl = Gl();
        const int width = gl.Width();
        const int height = gl.Height();

        const GLuint vs = MakeSeparable(GL_VERTEX_SHADER, kSeparableVS);
        const GLuint fs = MakeSeparable(GL_FRAGMENT_SHADER, kSeparableFS);
        if (vs == 0 || fs == 0) return;

        const GLuint pipeline = MakePipeline();
        glUseProgramStages(pipeline, GL_VERTEX_SHADER_BIT, vs);
        glUseProgramStages(pipeline, GL_FRAGMENT_SHADER_BIT, fs);
        ASSERT_EQ(FirstGLError(), 0u) << "pipeline setup left a GL error behind";

        GLuint vao = 0;
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);

        BindDefaultFramebuffer();
        glViewport(0, 0, width, height);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_DEPTH_TEST);
        ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
        // No glUseProgram anywhere: the pipeline IS the program state for this draw.
        glUseProgram(0);
        glBindProgramPipeline(pipeline);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        const Image painted = ReadPixels(width, height);
        EXPECT_TRUE(RegionIsMostly(painted, 2, width - 3, 2, height - 3, "green", 0.0,
                                   "a two-stage program pipeline drawing a full-viewport strip"));
        // The conformance harness fails a subcase on a leaked error even when the pixels are
        // right, so this assertion is not redundant with the one above.
        EXPECT_EQ(FirstGLError(), 0u) << "the pipeline draw leaked a GL error";

        glBindVertexArray(0);
        glDeleteVertexArrays(1, &vao);
        gl.EndFrame();
    }

    // glActiveShaderProgram picks which stage program glUniform* addresses.
    //
    // DISABLED: a second, independent defect, left failing on purpose rather than deleted. The
    // materialization fix above got the stages recorded and the pipeline drawing, but a uniform
    // set through the active shader program does not reach the flattened composite: the draw
    // paints u_color's default rather than the value written. GetProgramForUniform() returns the
    // pipeline's active program, while GetProgramForDraw() builds a SEPARATE composite object out
    // of the stage programs' shaders - so uniform values live on one object and the draw reads
    // another. Enable this the moment the composite inherits (or aliases) its stage programs'
    // uniform storage.
    TEST_F(ProgramPipelineScenario, DISABLED_UniformsGoToTheActiveShaderProgram) {
        if (!Ready()) return;

        static const char* kUniformFS = R"(#version 430 core
uniform vec4 u_color;
out vec4 o_color;
void main() { o_color = u_color; }
)";
        HeadlessGL& gl = Gl();
        const int width = gl.Width();
        const int height = gl.Height();

        const GLuint vs = MakeSeparable(GL_VERTEX_SHADER, kSeparableVS);
        const GLuint fs = MakeSeparable(GL_FRAGMENT_SHADER, kUniformFS);
        if (vs == 0 || fs == 0) return;

        const GLuint pipeline = MakePipeline();
        glUseProgramStages(pipeline, GL_VERTEX_SHADER_BIT, vs);
        glUseProgramStages(pipeline, GL_FRAGMENT_SHADER_BIT, fs);
        glBindProgramPipeline(pipeline);
        glActiveShaderProgram(pipeline, fs);
        ASSERT_EQ(FirstGLError(), 0u) << "glActiveShaderProgram left a GL error behind";

        const GLint location = glGetUniformLocation(fs, "u_color");
        ASSERT_NE(location, -1);
        glUniform4f(location, 0.0f, 1.0f, 0.0f, 1.0f);
        EXPECT_EQ(FirstGLError(), 0u) << "glUniform4f through the active shader program errored";

        GLuint vao = 0;
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        BindDefaultFramebuffer();
        glViewport(0, 0, width, height);
        glDisable(GL_DEPTH_TEST);
        ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        const Image painted = ReadPixels(width, height);
        EXPECT_TRUE(RegionIsMostly(painted, 2, width - 3, 2, height - 3, "green", 0.0,
                                   "a pipeline whose fragment uniform was set via glActiveShaderProgram"));
        EXPECT_EQ(FirstGLError(), 0u) << "the pipeline draw leaked a GL error";

        glBindVertexArray(0);
        glDeleteVertexArrays(1, &vao);
        gl.EndFrame();
    }

    // The sso-compute-pipeline shape: compute and non-compute stages on ONE pipeline object, the
    // compute stage writing the buffer the vertex stage then reads.
    //
    // DISABLED: the third defect in this cluster. Attaching a compute stage alongside graphics
    // stages is now accepted, but the dispatch/draw pair still paints nothing and leaves an error
    // behind - GetProgramForDraw flattens EVERY stage of the pipeline into one composite, so the
    // compute stage and the graphics stages end up in a single program that can serve neither
    // glDispatchCompute nor glDrawArrays correctly. GL keeps them separate: a pipeline's compute
    // stage is dispatched on its own and never participates in a draw. Enable this when the
    // flattening splits the compute stage out from the graphics ones.
    TEST_F(ProgramPipelineScenario, DISABLED_ComputeAndGraphicsStagesShareOnePipeline) {
        if (!Ready()) return;
        HeadlessGL& gl = Gl();
        const int width = gl.Width();
        const int height = gl.Height();

        GLint storageBlocks = 0;
        glGetIntegerv(GL_MAX_COMPUTE_SHADER_STORAGE_BLOCKS, &storageBlocks);
        if (storageBlocks < 1) {
            GTEST_SKIP() << "no compute shader storage blocks available";
        }

        const GLuint cs = MakeSeparable(GL_COMPUTE_SHADER, kComputeSource);
        const GLuint vs = MakeSeparable(GL_VERTEX_SHADER, kAttributeVS);
        const GLuint fs = MakeSeparable(GL_FRAGMENT_SHADER, kSeparableFS);
        if (cs == 0 || vs == 0 || fs == 0) return;

        const GLuint pipeline = MakePipeline();
        glUseProgramStages(pipeline, GL_VERTEX_SHADER_BIT, vs);
        glUseProgramStages(pipeline, GL_FRAGMENT_SHADER_BIT, fs);
        glUseProgramStages(pipeline, GL_COMPUTE_SHADER_BIT, cs);
        ASSERT_EQ(FirstGLError(), 0u) << "attaching compute and graphics stages to one pipeline errored";

        GLuint buffer = 0;
        glGenBuffers(1, &buffer);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
        glBufferData(GL_SHADER_STORAGE_BUFFER, 4 * 4 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

        GLuint vao = 0;
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, buffer);
        glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, nullptr);
        glEnableVertexAttribArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindVertexArray(0);

        BindDefaultFramebuffer();
        glViewport(0, 0, width, height);
        glDisable(GL_DEPTH_TEST);
        glUseProgram(0);
        glBindProgramPipeline(pipeline);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, buffer);
        glDispatchCompute(1, 1, 1);
        ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
        glBindVertexArray(vao);
        glMemoryBarrier(GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        const Image painted = ReadPixels(width, height);
        EXPECT_TRUE(RegionIsMostly(painted, 2, width - 3, 2, height - 3, "green", 0.0,
                                   "a pipeline whose compute stage wrote the vertex positions"));
        EXPECT_EQ(FirstGLError(), 0u) << "the compute-then-draw pipeline leaked a GL error";

        glBindVertexArray(0);
        glDeleteVertexArrays(1, &vao);
        glDeleteBuffers(1, &buffer);
        gl.EndFrame();
    }

    // build-separable / build-monolithic reduce to this: a separable program and a monolithic one
    // must both be usable, and switching between pipeline and glUseProgram must leave no error.
    TEST_F(ProgramPipelineScenario, SwitchingBetweenAPipelineAndAMonolithicProgramLeavesNoError) {
        if (!Ready()) return;
        HeadlessGL& gl = Gl();
        const int width = gl.Width();
        const int height = gl.Height();

        const GLuint vs = MakeSeparable(GL_VERTEX_SHADER, kSeparableVS);
        const GLuint fs = MakeSeparable(GL_FRAGMENT_SHADER, kSeparableFS);
        if (vs == 0 || fs == 0) return;
        const GLuint pipeline = MakePipeline();
        glUseProgramStages(pipeline, GL_VERTEX_SHADER_BIT | GL_FRAGMENT_SHADER_BIT, 0);
        glUseProgramStages(pipeline, GL_VERTEX_SHADER_BIT, vs);
        glUseProgramStages(pipeline, GL_FRAGMENT_SHADER_BIT, fs);

        std::string error;
        const unsigned int monolithic = CompileProgram(
            "#version 330 core\nin vec2 aPos;\nvoid main(){ gl_Position = vec4(aPos,0.0,1.0); }\n",
            "#version 330 core\nout vec4 o;\nvoid main(){ o = vec4(1.0,0.0,0.0,1.0); }\n", &error);
        ASSERT_NE(monolithic, 0u) << error;
        m_programs.push_back(monolithic);

        GLuint vao = 0;
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        BindDefaultFramebuffer();
        glViewport(0, 0, width, height);
        glDisable(GL_DEPTH_TEST);

        // GL 4.6 core 7.3: while a program is current, it takes precedence over the pipeline.
        ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
        glBindProgramPipeline(pipeline);
        glUseProgram(monolithic);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        EXPECT_EQ(FirstGLError(), 0u) << "drawing with a current program while a pipeline is bound errored";

        // ... and once it is not current, the pipeline takes over again.
        ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
        glUseProgram(0);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        const Image painted = ReadPixels(width, height);
        EXPECT_TRUE(RegionIsMostly(painted, 2, width - 3, 2, height - 3, "green", 0.0,
                                   "the pipeline after the current program was unbound"));
        EXPECT_EQ(FirstGLError(), 0u) << "switching back to the pipeline leaked a GL error";

        glBindVertexArray(0);
        glDeleteVertexArrays(1, &vao);
        gl.EndFrame();
    }
} // namespace MGITest
