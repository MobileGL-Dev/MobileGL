// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/SharedContextChangeScenario.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - A SHARED OBJECT CHANGED THROUGH ONE CONTEXT IS WHAT THE OTHER CONTEXT DRAWS WITH.
//
// Textures and renderbuffers belong to the share group (GL 4.6 core 5.1), so a change made
// through context B is the object context A sees, once B has finished and A has re-bound it
// (5.3.3's rule 4). The dirty walk's framebuffer and unit-set bits (11-14) used to shutter only on
// aggregates the change bumps on the context it is made THROUGH: B's glTexParameteri or
// glRenderbufferStorage moved nothing A's walk reads, A's re-bind of the same object is redundant
// and moves nothing either, and A went on recording the view set and framebuffer state from before
// the change. The share group's clocks (C8) are what A's walk now reads beside its own.
//
// Neither context draws between the change and the switch back, so the monolith's one tracker is
// still latched on A when A draws again - the shape where no context switch can rescue it.

#include <cstddef>
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

        constexpr int kFboSize = 32;
        constexpr int kTexSize = 8;
        constexpr int kSmallRenderbuffer = 8;

        constexpr const char* kQuadVertexSource = R"(#version 330 core
void main() {
    vec2 corner = vec2((gl_VertexID & 1) == 0 ? -1.0 : 1.0,
                       (gl_VertexID & 2) == 0 ? -1.0 : 1.0);
    gl_Position = vec4(corner, 0.0, 1.0);
}
)";

        // texelFetch: the point is WHETHER the texture is sampled, not how it is filtered.
        constexpr const char* kSampleFragmentSource = R"(#version 330 core
uniform sampler2D u_tex;
out vec4 o_color;
void main() {
    o_color = texelFetch(u_tex, ivec2(0, 0), 0);
}
)";

        constexpr const char* kGreenFragmentSource = R"(#version 330 core
out vec4 o_color;
void main() {
    o_color = vec4(0.0, 1.0, 0.0, 1.0);
}
)";

        class SharedContextChangeScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                glGenVertexArrays(1, &m_vao);
                ASSERT_EQ(FirstGLError(), 0u) << "the scene setup raised a GL error";
            }

            void TearDown() override {
                if (!Ready()) return;
                if (m_sibling != nullptr) Gl().DestroySharingContext(m_sibling);
                glBindVertexArray(0);
                glUseProgram(0);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, 0);
                if (m_vao != 0) glDeleteVertexArrays(1, &m_vao);
                ScenarioTest::TearDown();
            }

            // B, made current on the harness surface. Skips the case where EGL refuses one.
            bool SwitchToSibling() {
                if (m_sibling == nullptr) m_sibling = Gl().CreateSharingContext();
                if (m_sibling == nullptr) return false;
                return Gl().MakeContextCurrent(m_sibling);
            }
            static bool SwitchBack() { return Gl().MakeContextCurrent(nullptr); }

            void DrawQuad(GLuint program) {
                glBindVertexArray(m_vao);
                glUseProgram(program);
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                glBindVertexArray(0);
                EXPECT_EQ(FirstGLError(), 0u) << "the draw raised a GL error";
            }

            GLuint m_vao = 0;
            void* m_sibling = nullptr;
        };

    } // namespace

    // Bit 12: the view of an incomplete texture is null, and B's filter write completes it.
    TEST_F(SharedContextChangeScenario, ATextureCompletedThroughASharingContextIsSampledByTheOther) {
        if (!Ready() || IsSkipped()) return;
        std::string error;
        const unsigned int program = CompileProgram(kQuadVertexSource, kSampleFragmentSource, &error);
        ASSERT_NE(program, 0u) << error;
        ColorFbo target = MakeColorFbo(kFboSize, kFboSize);
        ASSERT_NE(target.fbo, 0u) << "could not create the render target";

        // ONE level and no glTexParameteri, so MIN_FILTER keeps GL_NEAREST_MIPMAP_LINEAR and the
        // texture is mipmap-incomplete: sampling it reads (0,0,0,1).
        GLuint texture = 0;
        glGenTextures(1, &texture);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture);
        std::vector<unsigned char> green(static_cast<std::size_t>(kTexSize * kTexSize * 4), 0);
        for (std::size_t i = 0; i < green.size(); i += 4) {
            green[i + 1] = 255;
            green[i + 3] = 255;
        }
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kTexSize, kTexSize, 0, GL_RGBA, GL_UNSIGNED_BYTE, green.data());
        glUseProgram(program);
        glUniform1i(glGetUniformLocation(program, "u_tex"), 0);
        ASSERT_EQ(FirstGLError(), 0u) << "defining the sampled texture raised a GL error";

        BindFbo(target);
        ClearTo(0.0f, 0.0f, 1.0f, 1.0f);
        DrawQuad(program);
        const Image incomplete = ReadPixels(kFboSize, kFboSize);
        ASSERT_TRUE(RegionIsMostly(incomplete, 0, kFboSize - 1, 0, kFboSize - 1, "black", 0.0,
                                   "an incomplete texture sampled through A"));

        // B completes the texture and finishes, and draws nothing.
        if (!SwitchToSibling()) GTEST_SKIP() << "EGL did not make a sharing context current";
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glFinish();
        ASSERT_EQ(FirstGLError(), 0u) << "completing the texture through B raised a GL error";

        // A re-binds it (rule 4), which is the same binding and moves nothing on A.
        ASSERT_TRUE(SwitchBack());
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture);
        BindFbo(target);
        ClearTo(0.0f, 0.0f, 1.0f, 1.0f);
        DrawQuad(program);
        const Image complete = ReadPixels(kFboSize, kFboSize);
        EXPECT_TRUE(RegionIsMostly(complete, 0, kFboSize - 1, 0, kFboSize - 1, "green", 0.0,
                                   "the same texture through A after B completed it"))
            << "black means A still recorded the null view of the incomplete texture";

        glBindTexture(GL_TEXTURE_2D, 0);
        glDeleteTextures(1, &texture);
        glDeleteProgram(program);
        DestroyColorFbo(target);
    }

    // Bit 11: set_framebuffer_state inlines the attachment's extent, and B resizes it.
    TEST_F(SharedContextChangeScenario, ARenderbufferResizedThroughASharingContextIsDrawnWhole) {
        if (!Ready() || IsSkipped()) return;
        std::string error;
        const unsigned int program = CompileProgram(kQuadVertexSource, kGreenFragmentSource, &error);
        ASSERT_NE(program, 0u) << error;

        GLuint renderbuffer = 0;
        glGenRenderbuffers(1, &renderbuffer);
        glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, kSmallRenderbuffer, kSmallRenderbuffer);
        GLuint fbo = 0;
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, renderbuffer);
        ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));
        glViewport(0, 0, kSmallRenderbuffer, kSmallRenderbuffer);
        ClearTo(0.0f, 0.0f, 1.0f, 1.0f);
        const Image small = ReadPixels(kSmallRenderbuffer, kSmallRenderbuffer);
        ASSERT_TRUE(RegionIsMostly(small, 0, kSmallRenderbuffer - 1, 0, kSmallRenderbuffer - 1, "blue", 0.0,
                                   "the small renderbuffer through A"));

        // B redefines the attached renderbuffer at four times the size, and draws nothing.
        if (!SwitchToSibling()) GTEST_SKIP() << "EGL did not make a sharing context current";
        glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, kFboSize, kFboSize);
        glFinish();
        ASSERT_EQ(FirstGLError(), 0u) << "resizing the renderbuffer through B raised a GL error";

        ASSERT_TRUE(SwitchBack());
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));
        glViewport(0, 0, kFboSize, kFboSize);
        ClearTo(0.0f, 0.0f, 1.0f, 1.0f);
        DrawQuad(program);
        const Image whole = ReadPixels(kFboSize, kFboSize);
        EXPECT_TRUE(RegionIsMostly(whole, 0, kFboSize - 1, 0, kFboSize - 1, "green", 0.0,
                                   "the resized renderbuffer through A"))
            << "a corner left uncovered means A still recorded the renderbuffer's old extent";

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &fbo);
        glDeleteRenderbuffers(1, &renderbuffer);
        glDeleteProgram(program);
    }

} // namespace MGITest
