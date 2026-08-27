// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/PrimitiveRestartScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - DESKTOP GL_PRIMITIVE_RESTART WITH AN APPLICATION-CHOSEN INDEX.
//
// Desktop GL restarts on whatever glPrimitiveRestartIndex named; GLES and Vulkan both restart
// only on the all-ones value of the index type. DirectGLES used to THROW_EXCEPTION on the
// mismatch, and a throw out of a GL entry point unwinds a C++ exception through the C ABI and
// kills the process - which is how KHR-GL4x.geometry_shader.primitive_counter.*_rp took the whole
// conformance runner down, nine bodies at a time, losing every result in the chunk with it.
//
// So the first thing this asserts is simply that the process is still here. The second is that
// the restart actually happened: the substitution rewrites the index data so the driver restarts
// where the application asked, and the difference between "restart honoured" and "restart
// silently dropped" is a triangle strip that welds its two halves together across the gap.
//
// Needs a real context on purpose. The GPU-free suite cannot reach a backend at all, and this is
// entirely about what the backend does with the index buffer.

#include <cstddef>
#include <iterator>
#include <string>
#include <utility>
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

        constexpr GLsizei kSurface = 64;

        const char* const kVertexSource = R"(#version 420 core
layout(location = 0) in vec2 a_position;
void main()
{
    gl_Position = vec4(a_position, 0.0, 1.0);
}
)";

        const char* const kFragmentSource = R"(#version 420 core
out vec4 fragColor;
void main()
{
    fragColor = vec4(0.0, 1.0, 0.0, 1.0);
}
)";

        // Two triangles with a gap down the middle, plus two spare vertices parked at the origin.
        //
        // The spares exist so the restart index is a LEGAL vertex index: if the restart were
        // dropped the driver would still fetch a real vertex rather than read out of bounds, so
        // the negative case is defined behaviour and the test measures the restart rather than
        // whatever robust-buffer-access does.
        constexpr GLfloat kVertices[] = {
            -0.9f, -0.9f, // 0 - left triangle
            -0.1f, -0.9f, // 1
            -0.9f,  0.9f, // 2
             0.1f, -0.9f, // 3 - right triangle
             0.9f, -0.9f, // 4
             0.9f,  0.9f, // 5
             0.0f,  0.0f, // 6 - spare
             0.0f,  0.0f, // 7 - spare, and the application's restart index
        };
        constexpr GLuint kRestartIndex = 7;

        // A triangle STRIP, restarted in the middle: honoured, it is exactly the two triangles
        // above. Dropped, the strip welds vertices 2, 7 and 3 into extra triangles that spill
        // across the gap - which is what the middle probe below catches.
        constexpr GLuint kIndices[] = {0, 1, 2, kRestartIndex, 3, 4, 5};

        struct Pixel {
            GLubyte r = 0, g = 0, b = 0, a = 0;
        };

        class PrimitiveRestartScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;

                glGenVertexArrays(1, &m_vao);
                glBindVertexArray(m_vao);

                glGenBuffers(1, &m_vbo);
                glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
                glBufferData(GL_ARRAY_BUFFER, sizeof(kVertices), kVertices, GL_STATIC_DRAW);
                glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(GLfloat), nullptr);
                glEnableVertexAttribArray(0);

                glGenBuffers(1, &m_ebo);
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ebo);
                glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(kIndices), kIndices, GL_STATIC_DRAW);

                glGenTextures(1, &m_colorTexture);
                glBindTexture(GL_TEXTURE_2D, m_colorTexture);
                glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, kSurface, kSurface);
                glGenFramebuffers(1, &m_fbo);
                glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_colorTexture, 0);
                ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER),
                          static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));
                glViewport(0, 0, kSurface, kSurface);

                m_program = BuildProgram();
                ASSERT_NE(m_program, 0u) << "the flat-colour program did not build: " << m_buildLog;
                glUseProgram(m_program);
                DrainErrors();
            }

            void TearDown() override {
                if (!Ready()) return;
                glDisable(GL_PRIMITIVE_RESTART);
                glDisable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
                glPrimitiveRestartIndex(0);
                glUseProgram(0);
                if (m_program != 0) glDeleteProgram(m_program);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                if (m_fbo != 0) glDeleteFramebuffers(1, &m_fbo);
                if (m_colorTexture != 0) glDeleteTextures(1, &m_colorTexture);
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
                glBindBuffer(GL_ARRAY_BUFFER, 0);
                if (m_ebo != 0) glDeleteBuffers(1, &m_ebo);
                if (m_vbo != 0) glDeleteBuffers(1, &m_vbo);
                glBindVertexArray(0);
                if (m_vao != 0) glDeleteVertexArrays(1, &m_vao);
                DrainErrors();
            }

            static void DrainErrors() {
                for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; ++i) {
                }
            }

            GLuint BuildProgram() {
                const GLuint vs = glCreateShader(GL_VERTEX_SHADER);
                glShaderSource(vs, 1, &kVertexSource, nullptr);
                glCompileShader(vs);
                const GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
                glShaderSource(fs, 1, &kFragmentSource, nullptr);
                glCompileShader(fs);
                const GLuint program = glCreateProgram();
                glAttachShader(program, vs);
                glAttachShader(program, fs);
                glLinkProgram(program);
                GLint linked = 0;
                glGetProgramiv(program, GL_LINK_STATUS, &linked);
                glDeleteShader(vs);
                glDeleteShader(fs);
                if (!linked) {
                    GLint length = 0;
                    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
                    std::vector<char> buffer(static_cast<std::size_t>(length) + 1, '\0');
                    glGetProgramInfoLog(program, length + 1, nullptr, buffer.data());
                    m_buildLog = buffer.data();
                    glDeleteProgram(program);
                    return 0;
                }
                return program;
            }

            // The whole surface, so a failure can report the three probes together rather than
            // three separate readbacks that might disagree about which draw they saw.
            std::vector<Pixel> DrawAndRead() {
                glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT);
                glDrawElements(GL_TRIANGLE_STRIP, static_cast<GLsizei>(std::size(kIndices)), GL_UNSIGNED_INT,
                               nullptr);
                std::vector<Pixel> pixels(static_cast<std::size_t>(kSurface) * kSurface);
                glReadPixels(0, 0, kSurface, kSurface, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
                return pixels;
            }

            static const Pixel& At(const std::vector<Pixel>& pixels, int x, int y) {
                return pixels[static_cast<std::size_t>(y) * kSurface + x];
            }

            static bool IsGreen(const Pixel& p) { return p.g > 128 && p.r < 128; }

            // NDC (-0.5, -0.5): well inside the left triangle whichever way the restart went.
            static constexpr int kLeftX = 16, kLeftY = 16;
            // NDC (0.6, -0.5): well inside the right triangle, and outside every welded one.
            static constexpr int kRightX = 51, kRightY = 16;
            // NDC (0.2, -0.5): in the gap between the two triangles, and INSIDE the triangle the
            // strip welds out of vertices 7, 3 and 4 when the restart is dropped. This is the
            // probe that distinguishes a working restart from a silently ignored one.
            static constexpr int kGapX = 38, kGapY = 16;

            GLuint m_vao = 0;
            GLuint m_vbo = 0;
            GLuint m_ebo = 0;
            GLuint m_fbo = 0;
            GLuint m_colorTexture = 0;
            GLuint m_program = 0;
            std::string m_buildLog;
        };

        // THE crash regression. Before the fix this call never returned: DirectGLES threw
        // std::runtime_error out of glDrawElements and the process died on the spot. Reaching the
        // assertion at all is most of the point.
        TEST_F(PrimitiveRestartScenario, AnArbitraryRestartIndexDrawsInsteadOfKillingTheProcess) {
            if (!Ready()) GTEST_SKIP();

            glEnable(GL_PRIMITIVE_RESTART);
            glPrimitiveRestartIndex(kRestartIndex);
            ASSERT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));

            const std::vector<Pixel> pixels = DrawAndRead();
            EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR))
                << "an arbitrary restart index is legal desktop GL and must raise no error";

            EXPECT_TRUE(IsGreen(At(pixels, kLeftX, kLeftY))) << "the first strip half did not render";
            EXPECT_TRUE(IsGreen(At(pixels, kRightX, kRightY))) << "the second strip half did not render";
            EXPECT_FALSE(IsGreen(At(pixels, kGapX, kGapY)))
                << "the gap between the two halves is covered, so the restart was dropped and the "
                   "strip welded across it";
        }

        // The other half of the state: an application that sets the restart index TO the fixed
        // all-ones value needs no rewriting at all, and the cap must map straight onto the
        // driver's own fixed-index restart. Same picture, different path through the backend.
        TEST_F(PrimitiveRestartScenario, TheFixedIndexValueTakesTheForwardingPath) {
            if (!Ready()) GTEST_SKIP();

            // Index 0xFFFFFFFF is not a vertex this draw uses, so the strip is the same shape.
            const GLuint fixedIndices[] = {0, 1, 2, 0xFFFFFFFFu, 3, 4, 5};
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ebo);
            glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, sizeof(fixedIndices), fixedIndices);

            glEnable(GL_PRIMITIVE_RESTART);
            glPrimitiveRestartIndex(0xFFFFFFFFu);
            ASSERT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));

            const std::vector<Pixel> pixels = DrawAndRead();
            EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
            EXPECT_TRUE(IsGreen(At(pixels, kLeftX, kLeftY)));
            EXPECT_TRUE(IsGreen(At(pixels, kRightX, kRightY)));
            EXPECT_FALSE(IsGreen(At(pixels, kGapX, kGapY)));

            // Put the buffer back for whatever runs next in this fixture.
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ebo);
            glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, sizeof(kIndices), kIndices);
            DrainErrors();
        }

        // With the cap off, the same index data is just data - nothing restarts, and the strip
        // welds across the gap. The negative control for the probe above: without it, a backend
        // that lost the whole draw would pass the test by rendering nothing in the gap.
        TEST_F(PrimitiveRestartScenario, WithoutTheCapTheStripWeldsAcrossTheGap) {
            if (!Ready()) GTEST_SKIP();

            glDisable(GL_PRIMITIVE_RESTART);
            glPrimitiveRestartIndex(kRestartIndex);
            ASSERT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));

            const std::vector<Pixel> pixels = DrawAndRead();
            EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
            EXPECT_TRUE(IsGreen(At(pixels, kLeftX, kLeftY))) << "the draw itself must still happen";
            EXPECT_TRUE(IsGreen(At(pixels, kGapX, kGapY)))
                << "with restart disabled the strip is continuous, so the gap must be covered - if "
                   "it is not, the probe above proves nothing";
        }

        // A second draw with a DIFFERENT restart index has to be rewritten again. The substitution
        // stages through one scratch buffer, so a cached or half-restored element-array binding
        // would show up here as the second draw reusing the first one's data.
        TEST_F(PrimitiveRestartScenario, ChangingTheRestartIndexBetweenDrawsIsHonoured) {
            if (!Ready()) GTEST_SKIP();

            glEnable(GL_PRIMITIVE_RESTART);
            glPrimitiveRestartIndex(kRestartIndex);
            const std::vector<Pixel> restarted = DrawAndRead();
            ASSERT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
            EXPECT_FALSE(IsGreen(At(restarted, kGapX, kGapY)));

            // 6 is the other spare vertex, and it appears nowhere in the index data - so nothing
            // restarts and the strip is continuous again, from the very same buffer.
            glPrimitiveRestartIndex(6);
            const std::vector<Pixel> notRestarted = DrawAndRead();
            ASSERT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
            EXPECT_TRUE(IsGreen(At(notRestarted, kLeftX, kLeftY)));
            EXPECT_TRUE(IsGreen(At(notRestarted, kGapX, kGapY)))
                << "the second draw restarted on an index that is not in its data";
        }

    } // namespace
} // namespace MGITest
