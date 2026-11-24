// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/MultiviewScenario.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - GL_OVR_multiview RENDERS EACH VIEW INTO ITS OWN LAYER.
//
// A two-view attachment of layers 1 and 2 of a three-layer array texture, read back one layer at
// a time through glFramebufferTextureLayer:
//
//  - a draw reaches both views, with gl_ViewID_OVR telling them apart in the vertex stage (its
//    position and a varying) and in the fragment stage;
//  - a clear reaches both views, and layer 0, outside the views, is never written;
//  - a scissored clear issued after a draw, while that draw's pass is still open, reaches both
//    views too.
//
// Skipped where the backend does not advertise GL_OVR_multiview2.

#include <cstring>
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
        constexpr int kSize = 32;

        // View 0 covers the whole target in red; view 1 only its left half, in green.
        constexpr const char* kVaryingVS = R"(#version 330 core
#extension GL_OVR_multiview2 : require
layout(num_views = 2) in;
layout(location = 0) in vec2 aPos;
out vec4 vColor;
void main() {
    bool second = gl_ViewID_OVR == 1u;
    vColor = second ? vec4(0.0, 1.0, 0.0, 1.0) : vec4(1.0, 0.0, 0.0, 1.0);
    float x = second ? aPos.x * 0.5 - 0.5 : aPos.x;
    gl_Position = vec4(x, aPos.y, 0.0, 1.0);
}
)";
        constexpr const char* kVaryingFS = R"(#version 330 core
in vec4 vColor;
out vec4 o_color;
void main() { o_color = vColor; }
)";

        constexpr const char* kPlainVS = R"(#version 330 core
#extension GL_OVR_multiview2 : require
layout(num_views = 2) in;
layout(location = 0) in vec2 aPos;
void main() { gl_Position = vec4(aPos, 0.0, 1.0); }
)";
        // The view seen from the fragment stage: red for view 0, green for view 1.
        constexpr const char* kViewFS = R"(#version 330 core
#extension GL_OVR_multiview2 : require
out vec4 o_color;
void main() { o_color = gl_ViewID_OVR == 1u ? vec4(0.0, 1.0, 0.0, 1.0) : vec4(1.0, 0.0, 0.0, 1.0); }
)";

        bool ExtensionAdvertised(const char* name) {
            GLint count = 0;
            glGetIntegerv(GL_NUM_EXTENSIONS, &count);
            for (GLint i = 0; i < count; ++i) {
                const auto* extension = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, i));
                if (extension && std::strcmp(extension, name) == 0) return true;
            }
            return false;
        }

        // llvmpipe's OpenGL ES driver corrupts its heap when a multiview framebuffer is cleared
        // through a scissor: the context's teardown then crashes, with or without MobileGL in the
        // process (reproduced on llvmpipe 25.2 with nothing but the clear and eglDestroyContext).
        bool DriverBreaksScissoredMultiviewClears() {
            const auto* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
            return renderer && std::strstr(renderer, "Espryt") && std::strstr(renderer, "llvmpipe");
        }

        void DrawFullScreen(unsigned int program) {
            const float quad[] = {-1.0f, -1.0f, 1.0f, -1.0f, 1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, -1.0f, 1.0f};
            GLuint vao = 0, vbo = 0;
            glGenVertexArrays(1, &vao);
            glBindVertexArray(vao);
            glGenBuffers(1, &vbo);
            glBindBuffer(GL_ARRAY_BUFFER, vbo);
            glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
            glUseProgram(program);
            glDrawArrays(GL_TRIANGLES, 0, 6);
            glUseProgram(0);
            glBindVertexArray(0);
            glDeleteBuffers(1, &vbo);
            glDeleteVertexArrays(1, &vao);
        }

        class MultiviewScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                if (!ExtensionAdvertised("GL_OVR_multiview2")) {
                    GTEST_SKIP() << "GL_OVR_multiview2 is not advertised by this backend/driver";
                }
                glGenTextures(1, &m_texture);
                glBindTexture(GL_TEXTURE_2D_ARRAY, m_texture);
                glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_RGBA8, kSize, kSize, 3);
                glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
                glGenFramebuffers(1, &m_readFbo);
                glGenFramebuffers(1, &m_multiviewFbo);
                // Every layer black first, one at a time, so layer 0 has a known value to keep.
                for (GLint layer = 0; layer < 3; ++layer) {
                    glBindFramebuffer(GL_FRAMEBUFFER, m_readFbo);
                    glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, m_texture, 0, layer);
                    ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
                }
                glBindFramebuffer(GL_FRAMEBUFFER, m_multiviewFbo);
                glFramebufferTextureMultiviewOVR(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, m_texture, 0, 1, 2);
                ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));
                glViewport(0, 0, kSize, kSize);
            }

            void TearDown() override {
                if (m_texture == 0) return;
                glDisable(GL_SCISSOR_TEST);
                BindDefaultFramebuffer();
                glDeleteFramebuffers(1, &m_multiviewFbo);
                glDeleteFramebuffers(1, &m_readFbo);
                glDeleteTextures(1, &m_texture);
            }

            Image ReadLayer(GLint layer) {
                glBindFramebuffer(GL_FRAMEBUFFER, m_readFbo);
                glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, m_texture, 0, layer);
                Image image = ReadPixels(kSize, kSize);
                glBindFramebuffer(GL_FRAMEBUFFER, m_multiviewFbo);
                return image;
            }

            ::testing::AssertionResult HalvesAre(GLint layer, const char* left, const char* right,
                                                 const std::string& when) {
                const Image image = ReadLayer(layer);
                const std::string where = when + ", layer " + std::to_string(layer);
                auto first = RegionIsMostly(image, 2, kSize / 2 - 3, 2, kSize - 3, left, 0.0, where + " (left half)");
                if (!first) return first;
                return RegionIsMostly(image, kSize / 2 + 2, kSize - 3, 2, kSize - 3, right, 0.0,
                                      where + " (right half)");
            }

            GLuint m_texture = 0;
            GLuint m_readFbo = 0;
            GLuint m_multiviewFbo = 0;
        };
    } // namespace

    TEST_F(MultiviewScenario, ADrawReachesEveryViewAndTheVertexStageTellsThemApart) {
        if (!Ready() || IsSkipped()) return;
        std::string error;
        const unsigned int program = CompileProgram(kVaryingVS, kVaryingFS, &error);
        ASSERT_NE(program, 0u) << error;
        ClearTo(0.0f, 0.0f, 1.0f, 1.0f);
        DrawFullScreen(program);
        EXPECT_EQ(FirstGLError(), 0u);
        EXPECT_TRUE(HalvesAre(0, "black", "black", "outside the views"));
        EXPECT_TRUE(HalvesAre(1, "red", "red", "view 0"));
        EXPECT_TRUE(HalvesAre(2, "green", "blue", "view 1"));
        glDeleteProgram(program);
        EXPECT_EQ(FirstGLError(), 0u);
    }

    TEST_F(MultiviewScenario, TheFragmentStageSeesItsView) {
        if (!Ready() || IsSkipped()) return;
        std::string error;
        const unsigned int program = CompileProgram(kPlainVS, kViewFS, &error);
        ASSERT_NE(program, 0u) << error;
        DrawFullScreen(program);
        EXPECT_EQ(FirstGLError(), 0u);
        EXPECT_TRUE(HalvesAre(1, "red", "red", "view 0"));
        EXPECT_TRUE(HalvesAre(2, "green", "green", "view 1"));
        glDeleteProgram(program);
        EXPECT_EQ(FirstGLError(), 0u);
    }

    TEST_F(MultiviewScenario, AScissoredClearAfterADrawReachesEveryView) {
        if (!Ready() || IsSkipped()) return;
        if (DriverBreaksScissoredMultiviewClears()) {
            GTEST_SKIP() << "llvmpipe's ES driver crashes after a scissored clear of a multiview framebuffer";
        }
        std::string error;
        const unsigned int program = CompileProgram(kPlainVS, kViewFS, &error);
        ASSERT_NE(program, 0u) << error;
        DrawFullScreen(program);
        glEnable(GL_SCISSOR_TEST);
        glScissor(kSize / 2, 0, kSize / 2, kSize);
        ClearTo(0.0f, 0.0f, 1.0f, 1.0f);
        glDisable(GL_SCISSOR_TEST);
        EXPECT_EQ(FirstGLError(), 0u);
        EXPECT_TRUE(HalvesAre(1, "red", "blue", "view 0"));
        EXPECT_TRUE(HalvesAre(2, "green", "blue", "view 1"));
        EXPECT_TRUE(HalvesAre(0, "black", "black", "outside the views"));
        glDeleteProgram(program);
        EXPECT_EQ(FirstGLError(), 0u);
    }
} // namespace MGITest
