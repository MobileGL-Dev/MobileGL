// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/UnboundUniformBlockScenario.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - A PROGRAM DECLARES A UNIFORM BLOCK AND NOTHING IS BOUND AT ITS BINDING POINT.
//
// The uniform-block sibling of UnboundImageDescriptorScenario. A draw or dispatch whose program
// declares a uniform block at an indexed point with no buffer on it raises no GL error - whether
// the point was never bound or its buffer was deleted while bound (glDeleteBuffers unbinds it
// from every indexed point of the current context). Only what the shader READS from the block is
// undefined.
//
// DirectVulkan's ResolveUniformBufferPayload looked up the buffer at the block's point and
// dereferenced it with nothing there: a SIGSEGV inside the draw or the dispatch, taking the whole
// process with it. It now answers with a zeroed block of the reflected size.
//
// The block is STATICALLY USED in every case, because an unreferenced block is optimised out
// before it ever reaches a descriptor and would prove nothing. Its one read sits behind a
// uniform-controlled branch that is false whenever the point is empty, so no undefined value
// reaches an assertion. Each case also binds a buffer at the same point and reads it back through
// the same program - proof that the block is live and wired to that point - and then empties the
// point again, so the transition back to "nothing bound" is covered as well as the first use.
//
// DirectGLES forwards the empty point to the GLES driver, which does what GL says, so it is the
// control - every test here must stay green on both backends.

#include <array>
#include <string>

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

        constexpr int kFboSize = 32;
        // The point under test. Not 0, so a stray glBindBufferBase(GL_UNIFORM_BUFFER, 0, ...) from
        // an earlier scenario in this process is not what decides the case - and SetUp empties
        // this one explicitly anyway.
        constexpr GLuint kUniformPoint = 5;
        constexpr GLuint kResultPoint = 0;
        constexpr unsigned int kMarker = 0xC0FFEEu;
        constexpr unsigned int kFallback = 7u;
        constexpr unsigned int kBoundWord = 0x1234u;

        // No vertex attributes: the quad's corners come from gl_VertexID, so nothing about the
        // vertex fetch can be confused with the descriptor question under test.
        constexpr const char* kQuadVertexSource = R"(#version 430 core
void main() {
    vec2 corner = vec2((gl_VertexID & 1) == 0 ? -1.0 : 1.0,
                       (gl_VertexID & 2) == 0 ? -1.0 : 1.0);
    gl_Position = vec4(corner, 0.0, 1.0);
}
)";

        // Green unless u_read is set, in which case the block's tint.
        constexpr const char* kFragmentSource = R"(#version 430 core
layout(std140, binding = 5) uniform Target { vec4 u_tint; };
uniform int u_read;
out vec4 o_color;
void main() {
    vec4 color = vec4(0.0, 1.0, 0.0, 1.0);
    if (u_read != 0) {
        color = u_tint;
    }
    o_color = color;
}
)";

        // The marker is unconditional, so a zero marker means the dispatch never ran; the value is
        // the fallback unless u_read is set, in which case it is the block's word.
        constexpr const char* kComputeSource = R"(#version 430 core
layout(local_size_x = 1) in;
layout(std430, binding = 0) buffer Result { uint g_marker; uint g_value; };
layout(std140, binding = 5) uniform Target { uint u_word; };
uniform int u_read;
void main() {
    uint value = 7u;
    if (u_read != 0) {
        value = u_word;
    }
    g_marker = 0xC0FFEEu;
    g_value = value;
}
)";

        class UnboundUniformBlockScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                m_target = MakeColorFbo(kFboSize, kFboSize);
                ASSERT_NE(m_target.fbo, 0u) << "could not create the render target";
                glGenVertexArrays(1, &m_vao);

                const unsigned int zero[2] = {0u, 0u};
                glGenBuffers(1, &m_result);
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_result);
                glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(zero), zero, GL_DYNAMIC_COPY);
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kResultPoint, m_result);

                // The harness shares one context across every scenario in the process, so an
                // earlier one may have left a buffer at this point. The whole subject here is that
                // nothing is bound, so say so rather than assume it.
                glBindBufferBase(GL_UNIFORM_BUFFER, kUniformPoint, 0);
                ASSERT_EQ(FirstGLError(), 0u) << "setting up the scenario raised a GL error";
            }

            void TearDown() override {
                if (!Ready()) return;
                glUseProgram(0);
                glBindVertexArray(0);
                glBindBufferBase(GL_UNIFORM_BUFFER, kUniformPoint, 0);
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kResultPoint, 0);
                if (m_block != 0) glDeleteBuffers(1, &m_block);
                if (m_result != 0) glDeleteBuffers(1, &m_result);
                if (m_vao != 0) glDeleteVertexArrays(1, &m_vao);
                if (m_draw != 0) glDeleteProgram(m_draw);
                if (m_compute != 0) glDeleteProgram(m_compute);
                BindDefaultFramebuffer();
                DestroyColorFbo(m_target);
                glViewport(0, 0, Gl().Width(), Gl().Height());
                FirstGLError();
            }

            unsigned int MakeComputeProgram(const char* source) {
                const GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
                glShaderSource(shader, 1, &source, nullptr);
                glCompileShader(shader);
                GLint compiled = GL_FALSE;
                glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
                if (compiled == GL_FALSE) {
                    char log[4096] = {};
                    glGetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
                    ADD_FAILURE() << "the compute shader did not compile: " << log;
                    glDeleteShader(shader);
                    return 0;
                }
                const GLuint program = glCreateProgram();
                glAttachShader(program, shader);
                glLinkProgram(program);
                glDeleteShader(shader);
                GLint linked = GL_FALSE;
                glGetProgramiv(program, GL_LINK_STATUS, &linked);
                if (linked == GL_FALSE) {
                    char log[4096] = {};
                    glGetProgramInfoLog(program, sizeof(log) - 1, nullptr, log);
                    ADD_FAILURE() << "the compute program did not link: " << log;
                    glDeleteProgram(program);
                    return 0;
                }
                return program;
            }

            // The block must be ACTIVE and at kUniformPoint, and the guard uniform must exist:
            // if either were optimised away the program would never ask for the empty point and
            // every case below would pass without testing anything.
            static void ExpectTheBlockIsLiveAtThePoint(GLuint program) {
                const GLuint block = glGetUniformBlockIndex(program, "Target");
                ASSERT_NE(block, GL_INVALID_INDEX) << "the uniform block was optimised out - the case proves nothing";
                GLint binding = -1;
                glGetActiveUniformBlockiv(program, block, GL_UNIFORM_BLOCK_BINDING, &binding);
                ASSERT_EQ(binding, static_cast<GLint>(kUniformPoint)) << "the block is not at the point under test";
                ASSERT_NE(glGetUniformLocation(program, "u_read"), -1) << "the guard uniform was optimised out";
                ASSERT_EQ(FirstGLError(), 0u) << "querying the block raised a GL error";
            }

            static void ExpectThePointIsEmpty(const char* when) {
                GLint bound = -1;
                glGetIntegeri_v(GL_UNIFORM_BUFFER_BINDING, kUniformPoint, &bound);
                ASSERT_EQ(bound, 0) << "uniform point " << kUniformPoint << " is not empty " << when;
            }

            // A 16-byte std140 block at kUniformPoint holding `words`.
            void BindBlock(const std::array<unsigned int, 4>& words) {
                if (m_block == 0) glGenBuffers(1, &m_block);
                glBindBuffer(GL_UNIFORM_BUFFER, m_block);
                glBufferData(GL_UNIFORM_BUFFER, sizeof(words), words.data(), GL_DYNAMIC_DRAW);
                glBindBuffer(GL_UNIFORM_BUFFER, 0);
                glBindBufferBase(GL_UNIFORM_BUFFER, kUniformPoint, m_block);
            }

            // Runs the compute program once; returns {marker, value}.
            std::array<unsigned int, 2> Dispatch(int read) {
                const unsigned int zero[2] = {0u, 0u};
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_result);
                glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(zero), zero);
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
                glUseProgram(m_compute);
                glUniform1i(glGetUniformLocation(m_compute, "u_read"), read);
                glDispatchCompute(1, 1, 1);
                glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
                std::array<unsigned int, 2> out = {0xDEADBEEFu, 0xDEADBEEFu};
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_result);
                glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(out), out.data());
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
                glUseProgram(0);
                return out;
            }

            void ExpectTheDispatchRunsOnAnEmptyPoint(const char* when) {
                ASSERT_NO_FATAL_FAILURE(ExpectThePointIsEmpty(when));
                const auto result = Dispatch(0);
                EXPECT_EQ(FirstGLError(), 0u) << "the dispatch raised a GL error " << when;
                EXPECT_EQ(result[0], kMarker) << "the dispatch did not run " << when;
                EXPECT_EQ(result[1], kFallback) << "the dispatch took the block's branch " << when;
            }

            // Draws the full-target quad and expects every pixel to be `color`.
            void ExpectTheDraw(int read, const char* color, const std::string& when) {
                BindFbo(m_target);
                ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
                glBindVertexArray(m_vao);
                glUseProgram(m_draw);
                glUniform1i(glGetUniformLocation(m_draw, "u_read"), read);
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                glBindVertexArray(0);
                glUseProgram(0);
                EXPECT_EQ(FirstGLError(), 0u) << "the draw raised a GL error " << when;

                const Image image = ReadPixels(kFboSize, kFboSize);
                ASSERT_FALSE(image.Empty()) << "the readback came back empty " << when;
                EXPECT_TRUE(RegionIsMostly(image, 0, kFboSize - 1, 0, kFboSize - 1, color, 0.0, when))
                    << "an all-black target means the draw was dropped";
            }

            void ExpectTheDrawRunsOnAnEmptyPoint(const std::string& when) {
                ASSERT_NO_FATAL_FAILURE(ExpectThePointIsEmpty(when.c_str()));
                ExpectTheDraw(0, "green", when);
            }

            ColorFbo m_target{};
            GLuint m_vao = 0;
            GLuint m_result = 0;
            GLuint m_block = 0;
            unsigned int m_draw = 0;
            unsigned int m_compute = 0;
        };

    } // namespace

    TEST_F(UnboundUniformBlockScenario, ADispatchDeclaringAnEmptyUniformPointRuns) {
        if (!Ready() || IsSkipped()) return;
        m_compute = MakeComputeProgram(kComputeSource);
        ASSERT_NE(m_compute, 0u);
        ASSERT_NO_FATAL_FAILURE(ExpectTheBlockIsLiveAtThePoint(m_compute));

        ExpectTheDispatchRunsOnAnEmptyPoint("with nothing ever bound at the point");

        BindBlock({kBoundWord, 0u, 0u, 0u});
        const auto bound = Dispatch(1);
        EXPECT_EQ(FirstGLError(), 0u);
        EXPECT_EQ(bound[0], kMarker) << "the dispatch did not run with a buffer bound";
        EXPECT_EQ(bound[1], kBoundWord) << "the program did not read the buffer bound at the block's point";

        glBindBufferBase(GL_UNIFORM_BUFFER, kUniformPoint, 0);
        ExpectTheDispatchRunsOnAnEmptyPoint("after the point was emptied again");
    }

    TEST_F(UnboundUniformBlockScenario, ADrawDeclaringAnEmptyUniformPointPaintsTheFrame) {
        if (!Ready() || IsSkipped()) return;
        std::string error;
        m_draw = CompileProgram(kQuadVertexSource, kFragmentSource, &error);
        ASSERT_NE(m_draw, 0u) << error;
        ASSERT_NO_FATAL_FAILURE(ExpectTheBlockIsLiveAtThePoint(m_draw));

        ExpectTheDrawRunsOnAnEmptyPoint("with nothing ever bound at the point");

        // Opaque red: 1.0f and 0.0f as their IEEE-754 words, so the block is one vec4.
        BindBlock({0x3F800000u, 0u, 0u, 0x3F800000u});
        ExpectTheDraw(1, "red", "reading the buffer bound at the block's point");

        glBindBufferBase(GL_UNIFORM_BUFFER, kUniformPoint, 0);
        ExpectTheDrawRunsOnAnEmptyPoint("after the point was emptied again");
    }

    // The shape real applications reach it by: the buffer is bound and used, then deleted while
    // still bound, which empties the point without the application ever unbinding it.
    TEST_F(UnboundUniformBlockScenario, AUniformBufferDeletedWhileBoundLeavesAnEmptyPointBothPathsSurvive) {
        if (!Ready() || IsSkipped()) return;
        m_compute = MakeComputeProgram(kComputeSource);
        ASSERT_NE(m_compute, 0u);
        ASSERT_NO_FATAL_FAILURE(ExpectTheBlockIsLiveAtThePoint(m_compute));
        std::string error;
        m_draw = CompileProgram(kQuadVertexSource, kFragmentSource, &error);
        ASSERT_NE(m_draw, 0u) << error;
        ASSERT_NO_FATAL_FAILURE(ExpectTheBlockIsLiveAtThePoint(m_draw));

        BindBlock({kBoundWord, 0u, 0u, 0u});
        const auto bound = Dispatch(1);
        EXPECT_EQ(bound[0], kMarker) << "the dispatch did not run with a buffer bound";
        EXPECT_EQ(bound[1], kBoundWord) << "the program did not read the buffer bound at the block's point";
        ASSERT_EQ(FirstGLError(), 0u);

        glDeleteBuffers(1, &m_block);
        m_block = 0;
        ExpectTheDispatchRunsOnAnEmptyPoint("after the bound buffer was deleted");
        ExpectTheDrawRunsOnAnEmptyPoint("after the bound buffer was deleted");
    }

} // namespace MGITest
