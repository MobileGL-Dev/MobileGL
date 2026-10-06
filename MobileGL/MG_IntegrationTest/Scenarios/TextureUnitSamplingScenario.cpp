// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/TextureUnitSamplingScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// P13 W6: WHAT A DRAW SAMPLES FROM A TEXTURE UNIT, ON A REAL CONTEXT. Two GL 4.6 rules that were
// pinned by DirectGLESSanity unit cases driving the monolith frontend arm's unit walk
// (BindCurrentTextures over the frontend's binding slots). That arm is gone - every library walks
// the units by handle now - so the rules are asserted here, through the whole pipe, on both
// backends and every arm the scenarios run on:
//
//   * A multisample texture is fetched, never filtered, so the mip-chain completeness rules do
//     not apply to it (GL 4.6 8.17) even though its MIN_FILTER keeps the initial
//     NEAREST_MIPMAP_LINEAR. Calling it incomplete leaves the unit unbound, and every texelFetch
//     reads zero from a texture that was written correctly (KHR-GL43.compute_shader.resource-
//     texture's shape: clear through an FBO, then read through sampler2DMS).
//   * Binding texture 0 to a unit samples the unit's default texture, which has no image and is
//     therefore incomplete: (0, 0, 0, 1) (GL 4.6 8.17.1). The texture bound there before must not
//     keep answering.

#include <array>
#include <cstdint>
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

        constexpr GLsizei kEdge = 4;

        constexpr const char* kVertexSource = R"(#version 330 core
void main() {
    vec2 corner = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
)";

        constexpr const char* kFetchMultisampleSource = R"(#version 330 core
uniform sampler2DMS u_texture;
out vec4 o_color;
void main() { o_color = texelFetch(u_texture, ivec2(gl_FragCoord.xy), 0); }
)";

        constexpr const char* kSample2DSource = R"(#version 330 core
uniform sampler2D u_texture;
out vec4 o_color;
void main() { o_color = texture(u_texture, vec2(0.5, 0.5)); }
)";

        class TextureUnitSamplingScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                DrainErrors();
                glGenVertexArrays(1, &m_vao);
                glBindVertexArray(m_vao);
                glGenTextures(1, &m_target);
                glBindTexture(GL_TEXTURE_2D, m_target);
                glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, kEdge, kEdge);
                glBindTexture(GL_TEXTURE_2D, 0);
                glGenFramebuffers(1, &m_fbo);
                glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_target, 0);
                glViewport(0, 0, kEdge, kEdge);
                glDisable(GL_SCISSOR_TEST);
                glDisable(GL_BLEND);
                glDisable(GL_DEPTH_TEST);
                glPixelStorei(GL_PACK_ALIGNMENT, 4);
            }

            void TearDown() override {
                if (!Ready()) return;
                glUseProgram(0);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                if (m_fbo != 0) glDeleteFramebuffers(1, &m_fbo);
                if (m_msFbo != 0) glDeleteFramebuffers(1, &m_msFbo);
                if (m_target != 0) glDeleteTextures(1, &m_target);
                if (m_source != 0) glDeleteTextures(1, &m_source);
                if (m_program != 0) glDeleteProgram(m_program);
                if (m_vao != 0) glDeleteVertexArrays(1, &m_vao);
                DrainErrors();
                ScenarioTest::TearDown();
            }

            static void DrainErrors() {
                for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; ++i) {
                }
            }

            static GLuint Compile(GLenum stage, const char* source) {
                const GLuint shader = glCreateShader(stage);
                glShaderSource(shader, 1, &source, nullptr);
                glCompileShader(shader);
                GLint ok = GL_FALSE;
                glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
                if (ok != GL_TRUE) {
                    char log[1024] = {};
                    glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
                    ADD_FAILURE() << "shader compile failed: " << log;
                }
                return shader;
            }

            void UseProgram(const char* fragmentSource) {
                const GLuint vs = Compile(GL_VERTEX_SHADER, kVertexSource);
                const GLuint fs = Compile(GL_FRAGMENT_SHADER, fragmentSource);
                m_program = glCreateProgram();
                glAttachShader(m_program, vs);
                glAttachShader(m_program, fs);
                glLinkProgram(m_program);
                glDeleteShader(vs);
                glDeleteShader(fs);
                GLint ok = GL_FALSE;
                glGetProgramiv(m_program, GL_LINK_STATUS, &ok);
                ASSERT_EQ(ok, GL_TRUE) << "program link failed";
                glUseProgram(m_program);
                glUniform1i(glGetUniformLocation(m_program, "u_texture"), 0);
            }

            std::array<GLubyte, 4> DrawAndReadCentre() {
                glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
                glClearColor(0.5f, 0.5f, 0.5f, 0.5f);
                glClear(GL_COLOR_BUFFER_BIT);
                glDrawArrays(GL_TRIANGLES, 0, 3);
                std::array<GLubyte, 4> texel{};
                glReadPixels(kEdge / 2, kEdge / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, texel.data());
                return texel;
            }

            GLuint m_vao = 0;
            GLuint m_fbo = 0;
            GLuint m_msFbo = 0;
            GLuint m_target = 0;
            GLuint m_source = 0;
            GLuint m_program = 0;
        };

        TEST_F(TextureUnitSamplingScenario, AMultisampleTextureIsFetchedDespiteItsDefaultMipmapFilter) {
            if (!Ready()) return;
            glGenTextures(1, &m_source);
            glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, m_source);
            glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, 4, GL_RGBA8, kEdge, kEdge, GL_FALSE);
            GLint minFilter = 0;
            glGetTexParameteriv(GL_TEXTURE_2D_MULTISAMPLE, GL_TEXTURE_MIN_FILTER, &minFilter);
            ASSERT_EQ(minFilter, GL_NEAREST_MIPMAP_LINEAR)
                << "the fixture is stale: the initial MIN_FILTER no longer asks for mipmaps";

            glGenFramebuffers(1, &m_msFbo);
            glBindFramebuffer(GL_FRAMEBUFFER, m_msFbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D_MULTISAMPLE, m_source, 0);
            ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));
            glClearColor(1.0f, 0.0f, 1.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            ASSERT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR)) << "clearing the multisample texture failed";

            UseProgram(kFetchMultisampleSource);
            if (HasFatalFailure()) return;
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, m_source);
            const auto texel = DrawAndReadCentre();
            EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
            EXPECT_EQ(static_cast<int>(texel[0]), 255) << "red: the multisample texture was not bound";
            EXPECT_EQ(static_cast<int>(texel[1]), 0);
            EXPECT_EQ(static_cast<int>(texel[2]), 255) << "blue: the multisample texture was not bound";
            EXPECT_EQ(static_cast<int>(texel[3]), 255);
            Gl().EndFrame();
        }

        TEST_F(TextureUnitSamplingScenario, BindingZeroSamplesTheIncompleteDefaultTexture) {
            if (!Ready()) return;
            glGenTextures(1, &m_source);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, m_source);
            const GLubyte red[4] = {255, 0, 0, 255};
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, red);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            UseProgram(kSample2DSource);
            if (HasFatalFailure()) return;

            const auto bound = DrawAndReadCentre();
            ASSERT_EQ(static_cast<int>(bound[0]), 255) << "the control: the bound texture is sampled";
            ASSERT_EQ(static_cast<int>(bound[1]), 0);

            glBindTexture(GL_TEXTURE_2D, 0);
            const auto unbound = DrawAndReadCentre();
            EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
            EXPECT_EQ(static_cast<int>(unbound[0]), 0)
                << "texture 0 is bound, yet the unit still answers with the texture bound before it";
            EXPECT_EQ(static_cast<int>(unbound[1]), 0);
            EXPECT_EQ(static_cast<int>(unbound[2]), 0);
            EXPECT_EQ(static_cast<int>(unbound[3]), 255) << "an incomplete texture samples as (0, 0, 0, 1)";
            Gl().EndFrame();
        }

    } // namespace
} // namespace MGITest
