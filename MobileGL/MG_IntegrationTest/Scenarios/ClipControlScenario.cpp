// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/ClipControlScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - glClipControl REACHES THE RASTERIZER, AND ONLY THE APPLICATION'S DRAWS.
//
// The state used to be recorded and reported back by glGet but never applied, so an upper-left
// origin drew the right way up and a [0, 1] depth mode kept GL's [-1, 1] clip volume. Each case
// below is a colour-only observable of one half of the extension:
//
//  - origin: a primitive in the lower half of clip space lands in the TOP half of the window
//    under GL_UPPER_LEFT, on the default framebuffer (which DirectVulkan stores flipped already)
//    and on an FBO (which it does not);
//  - facing: culling follows the clip-space winding whatever the origin, as the extension
//    requires;
//  - depth: under GL_ZERO_TO_ONE a primitive at z = -0.5 is outside the clip volume, and one at
//    z = 0.25 is written as 0.25 rather than 0.625;
//  - a framebuffer blit issued while the application's origin is upper-left is a copy, not a
//    draw, and must not come out mirrored.

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

        constexpr const char* kVS = R"(#version 330 core
in vec3 aPos;
void main() { gl_Position = vec4(aPos, 1.0); }
)";

        constexpr const char* kFS = R"(#version 330 core
out vec4 o_color;
void main() { o_color = vec4(1.0, 0.0, 0.0, 1.0); }
)";

        // Red where the rasterizer calls the fragment front-facing, green where it calls it back-facing.
        constexpr const char* kFacingFS = R"(#version 330 core
out vec4 o_color;
void main() { o_color = gl_FrontFacing ? vec4(1.0, 0.0, 0.0, 1.0) : vec4(0.0, 1.0, 0.0, 1.0); }
)";

        // Left half of clip space counter-clockwise, right half clockwise.
        constexpr float kWoundHalves[] = {
            -1.0f, -1.0f, 0.0f, 0.0f, -1.0f, 0.0f, -1.0f, 1.0f,  0.0f, //
            0.0f,  -1.0f, 0.0f, 0.0f, 1.0f,  0.0f, -1.0f, 1.0f,  0.0f, //
            0.0f,  -1.0f, 0.0f, 0.0f, 1.0f,  0.0f, 1.0f,  -1.0f, 0.0f, //
            1.0f,  -1.0f, 0.0f, 0.0f, 1.0f,  0.0f, 1.0f,  1.0f,  0.0f,
        };

        bool ExtensionAdvertised(const char* name) {
            GLint count = 0;
            glGetIntegerv(GL_NUM_EXTENSIONS, &count);
            for (GLint i = 0; i < count; ++i) {
                const auto* extension = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, i));
                if (extension && std::strcmp(extension, name) == 0) return true;
            }
            return false;
        }

        // Draws `vertexCount` xyz vertices as GL_TRIANGLES in red.
        void DrawTriangles(unsigned int program, const float* xyz, int vertexCount) {
            GLuint vao = 0, vbo = 0;
            glGenVertexArrays(1, &vao);
            glBindVertexArray(vao);
            glGenBuffers(1, &vbo);
            glBindBuffer(GL_ARRAY_BUFFER, vbo);
            glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertexCount * 3 * sizeof(float)), xyz,
                         GL_STATIC_DRAW);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);
            glUseProgram(program);
            glDrawArrays(GL_TRIANGLES, 0, vertexCount);
            glUseProgram(0);
            glBindVertexArray(0);
            glDeleteBuffers(1, &vbo);
            glDeleteVertexArrays(1, &vao);
        }

        // The lower half of clip space (y in [-1, 0]) as two triangles at depth z.
        void DrawLowerHalf(unsigned int program, float z = 0.0f) {
            const float quad[] = {-1.0f, -1.0f, z, 1.0f, -1.0f, z, 1.0f,  0.0f, z,
                                  -1.0f, -1.0f, z, 1.0f, 0.0f,  z, -1.0f, 0.0f, z};
            DrawTriangles(program, quad, 6);
        }

        // The whole of clip space at depth z.
        void DrawEverything(unsigned int program, float z) {
            const float quad[] = {-1.0f, -1.0f, z, 1.0f, -1.0f, z, 1.0f,  1.0f, z,
                                  -1.0f, -1.0f, z, 1.0f, 1.0f,  z, -1.0f, 1.0f, z};
            DrawTriangles(program, quad, 6);
        }

        // Bottom and top halves of a `width` x `height` readback, inset away from the seam.
        ::testing::AssertionResult HalvesAre(const Image& image, const char* bottom, const char* top,
                                             const std::string& when) {
            const int w = image.Width(), h = image.Height();
            auto lower = RegionIsMostly(image, w / 8, w - 1 - w / 8, h / 8, h / 2 - 1 - h / 8, bottom, 0.0,
                                        when + " (bottom half)");
            if (!lower) return lower;
            return RegionIsMostly(image, w / 8, w - 1 - w / 8, h / 2 + h / 8, h - 1 - h / 8, top, 0.0,
                                  when + " (top half)");
        }

        class ClipControlScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                if (!ExtensionAdvertised("GL_ARB_clip_control")) {
                    GTEST_SKIP() << "GL_ARB_clip_control is not advertised by this backend/driver";
                }
                std::string error;
                m_program = CompileProgram(kVS, kFS, &error);
                ASSERT_NE(m_program, 0u) << error;
            }

            void TearDown() override {
                if (m_program == 0) return;
                glClipControl(GL_LOWER_LEFT, GL_NEGATIVE_ONE_TO_ONE);
                glDisable(GL_CULL_FACE);
                glDisable(GL_DEPTH_TEST);
                glDeleteProgram(m_program);
            }

            unsigned int m_program = 0;
        };

    } // namespace

    TEST_F(ClipControlScenario, UpperLeftOriginMirrorsTheImageOnBothFramebufferKinds) {
        if (!Ready() || IsSkipped()) return;
        HeadlessGL& gl = Gl();
        ColorFbo fbo = MakeColorFbo(64, 64);
        ASSERT_NE(fbo.fbo, 0u);

        for (const bool toDefault : {true, false}) {
            for (const GLenum origin : {GLenum(GL_LOWER_LEFT), GLenum(GL_UPPER_LEFT)}) {
                if (toDefault) {
                    BindDefaultFramebuffer();
                } else {
                    BindFbo(fbo);
                }
                glDisable(GL_SCISSOR_TEST);
                ClearTo(0.0f, 0.0f, 1.0f, 1.0f);
                glClipControl(origin, GL_NEGATIVE_ONE_TO_ONE);
                GLint reported = 0;
                glGetIntegerv(GL_CLIP_ORIGIN, &reported);
                EXPECT_EQ(static_cast<GLenum>(reported), origin);
                DrawLowerHalf(m_program);
                glClipControl(GL_LOWER_LEFT, GL_NEGATIVE_ONE_TO_ONE);

                const Image image = toDefault ? ReadPixels(gl.Width(), gl.Height()) : ReadPixels(64, 64);
                const std::string when = std::string(toDefault ? "default framebuffer" : "FBO") +
                                         (origin == GL_UPPER_LEFT ? ", GL_UPPER_LEFT" : ", GL_LOWER_LEFT");
                if (origin == GL_UPPER_LEFT) {
                    EXPECT_TRUE(HalvesAre(image, "blue", "red", when));
                } else {
                    EXPECT_TRUE(HalvesAre(image, "red", "blue", when));
                }
            }
        }
        BindDefaultFramebuffer();
        DestroyColorFbo(fbo);
        EXPECT_EQ(FirstGLError(), 0u);
    }

    TEST_F(ClipControlScenario, CullingFollowsTheClipSpaceWindingUnderEitherOrigin) {
        if (!Ready() || IsSkipped()) return;
        ColorFbo fbo = MakeColorFbo(64, 64);
        ASSERT_NE(fbo.fbo, 0u);
        const float* triangles = kWoundHalves;
        for (const bool toDefault : {true, false}) {
            for (const GLenum origin : {GLenum(GL_LOWER_LEFT), GLenum(GL_UPPER_LEFT)}) {
                if (toDefault) {
                    BindDefaultFramebuffer();
                } else {
                    BindFbo(fbo);
                }
                ClearTo(0.0f, 0.0f, 1.0f, 1.0f);
                glEnable(GL_CULL_FACE); // GL_BACK, GL_CCW
                glClipControl(origin, GL_NEGATIVE_ONE_TO_ONE);
                DrawTriangles(m_program, triangles, 12);
                glClipControl(GL_LOWER_LEFT, GL_NEGATIVE_ONE_TO_ONE);
                glDisable(GL_CULL_FACE);

                const Image image = toDefault ? ReadPixels(Gl().Width(), Gl().Height()) : ReadPixels(64, 64);
                const int w = image.Width(), h = image.Height();
                const std::string when = std::string(toDefault ? "default framebuffer" : "FBO") +
                                         (origin == GL_UPPER_LEFT ? ", GL_UPPER_LEFT" : ", GL_LOWER_LEFT");
                EXPECT_TRUE(RegionIsMostly(image, w / 8, w / 2 - 1 - w / 8, h / 8, h - 1 - h / 8, "red", 0.0,
                                           when + ": counter-clockwise half is front-facing"));
                EXPECT_TRUE(RegionIsMostly(image, w / 2 + w / 8, w - 1 - w / 8, h / 8, h - 1 - h / 8, "blue", 0.0,
                                           when + ": clockwise half is culled"));
            }
        }
        BindDefaultFramebuffer();
        DestroyColorFbo(fbo);
        EXPECT_EQ(FirstGLError(), 0u);
    }

    // gl_FrontFacing has to agree with culling, under either origin, on either framebuffer kind,
    // and has to follow glFrontFace. A backend that keeps facing right by swapping its cull bits
    // instead of its front face gets culling right and gl_FrontFacing backwards.
    TEST_F(ClipControlScenario, FrontFacingFollowsTheWindingAndGlFrontFace) {
        if (!Ready() || IsSkipped()) return;
        std::string error;
        const unsigned int facing = CompileProgram(kVS, kFacingFS, &error);
        ASSERT_NE(facing, 0u) << error;
        ColorFbo fbo = MakeColorFbo(64, 64);
        ASSERT_NE(fbo.fbo, 0u);
        for (const bool toDefault : {true, false}) {
            for (const GLenum origin : {GLenum(GL_LOWER_LEFT), GLenum(GL_UPPER_LEFT)}) {
                for (const GLenum frontFace : {GLenum(GL_CCW), GLenum(GL_CW)}) {
                    if (toDefault) {
                        BindDefaultFramebuffer();
                    } else {
                        BindFbo(fbo);
                    }
                    ClearTo(0.0f, 0.0f, 1.0f, 1.0f);
                    glFrontFace(frontFace);
                    glClipControl(origin, GL_NEGATIVE_ONE_TO_ONE);
                    DrawTriangles(facing, kWoundHalves, 12);
                    glClipControl(GL_LOWER_LEFT, GL_NEGATIVE_ONE_TO_ONE);
                    glFrontFace(GL_CCW);

                    const Image image = toDefault ? ReadPixels(Gl().Width(), Gl().Height()) : ReadPixels(64, 64);
                    const int w = image.Width(), h = image.Height();
                    const std::string when = std::string(toDefault ? "default framebuffer" : "FBO") +
                                             (origin == GL_UPPER_LEFT ? ", GL_UPPER_LEFT" : ", GL_LOWER_LEFT") +
                                             (frontFace == GL_CW ? ", GL_CW" : ", GL_CCW");
                    const char* ccwColor = frontFace == GL_CCW ? "red" : "green";
                    const char* cwColor = frontFace == GL_CCW ? "green" : "red";
                    EXPECT_TRUE(RegionIsMostly(image, w / 8, w / 2 - 1 - w / 8, h / 8, h - 1 - h / 8, ccwColor, 0.0,
                                               when + ": counter-clockwise half"));
                    EXPECT_TRUE(RegionIsMostly(image, w / 2 + w / 8, w - 1 - w / 8, h / 8, h - 1 - h / 8, cwColor, 0.0,
                                               when + ": clockwise half"));
                }
            }
        }
        BindDefaultFramebuffer();
        DestroyColorFbo(fbo);
        glDeleteProgram(facing);
        EXPECT_EQ(FirstGLError(), 0u);
    }

    TEST_F(ClipControlScenario, ZeroToOneDepthModeMovesTheClipVolumeAndTheMapping) {
        if (!Ready() || IsSkipped()) return;
        ColorFbo fbo = MakeColorFbo(64, 64);
        ASSERT_NE(fbo.fbo, 0u);
        GLuint depth = 0;
        glGenRenderbuffers(1, &depth);
        glBindRenderbuffer(GL_RENDERBUFFER, depth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, 64, 64);
        BindFbo(fbo);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
        ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));

        for (const GLenum mode : {GLenum(GL_NEGATIVE_ONE_TO_ONE), GLenum(GL_ZERO_TO_ONE)}) {
            const bool zeroToOne = mode == GL_ZERO_TO_ONE;
            const std::string when = zeroToOne ? "GL_ZERO_TO_ONE" : "GL_NEGATIVE_ONE_TO_ONE";

            // z = -0.5 is inside [-w, w] and outside [0, w].
            glDisable(GL_DEPTH_TEST);
            ClearTo(0.0f, 0.0f, 1.0f, 1.0f);
            glClipControl(GL_LOWER_LEFT, mode);
            GLint reported = 0;
            glGetIntegerv(GL_CLIP_DEPTH_MODE, &reported);
            EXPECT_EQ(static_cast<GLenum>(reported), mode);
            DrawEverything(m_program, -0.5f);
            EXPECT_TRUE(RegionIsMostly(ReadPixels(64, 64), 8, 55, 8, 55, zeroToOne ? "blue" : "red", 0.0,
                                       when + ": a primitive at z = -0.5"));

            // Against a depth buffer cleared to 0.5: z = 0.25 is written as 0.25 under [0, 1] and as
            // 0.625 under [-1, 1].
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_LESS);
            glDepthMask(GL_TRUE);
            glClearDepth(0.5);
            glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            DrawEverything(m_program, 0.25f);
            EXPECT_TRUE(RegionIsMostly(ReadPixels(64, 64), 8, 55, 8, 55, zeroToOne ? "red" : "blue", 0.0,
                                       when + ": a primitive at z = 0.25 against depth 0.5"));
            glClipControl(GL_LOWER_LEFT, GL_NEGATIVE_ONE_TO_ONE);
        }
        glDisable(GL_DEPTH_TEST);
        glClearDepth(1.0);
        BindDefaultFramebuffer();
        glDeleteRenderbuffers(1, &depth);
        DestroyColorFbo(fbo);
        EXPECT_EQ(FirstGLError(), 0u);
    }

    TEST_F(ClipControlScenario, ABlitUnderAnUpperLeftOriginIsNotMirrored) {
        if (!Ready() || IsSkipped()) return;
        HeadlessGL& gl = Gl();
        ColorFbo fbo = MakeColorFbo(64, 64);
        ASSERT_NE(fbo.fbo, 0u);
        BindFbo(fbo);
        ClearTo(0.0f, 0.0f, 1.0f, 1.0f);
        DrawLowerHalf(m_program);

        glClipControl(GL_UPPER_LEFT, GL_ZERO_TO_ONE);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo.fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glBlitFramebuffer(0, 0, 64, 64, 0, 0, gl.Width(), gl.Height(), GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glClipControl(GL_LOWER_LEFT, GL_NEGATIVE_ONE_TO_ONE);

        BindDefaultFramebuffer();
        EXPECT_TRUE(HalvesAre(ReadPixels(gl.Width(), gl.Height()), "red", "blue", "FBO blitted to the window"));
        DestroyColorFbo(fbo);
        EXPECT_EQ(FirstGLError(), 0u);
    }

} // namespace MGITest
