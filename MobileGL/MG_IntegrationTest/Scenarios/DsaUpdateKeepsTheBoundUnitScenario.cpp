// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/DsaUpdateKeepsTheBoundUnitScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - A BY-NAME (DSA) UPDATE OF TEXTURE B DOES NOT RETARGET THE UNIT THAT HOLDS TEXTURE A.
//
// GL 4.6 core 8.1/8.5: glTextureSubImage2D / glTextureStorage2D name their texture directly and
// change no binding. So with A bound to unit 0, a by-name update of B must leave every later draw
// that samples unit 0 sampling A - A's texels and A's shape.
//
// The regression this guards (P13 W3b; it used to be the white-box SanityTest probe
// DirectGLESTextureSync.UnitMemoRefusesToDriveATwinFromAnotherTexture): Espryt's per-unit texture
// sync list on the monolith arm BORROWS a pointer into the frontend binding slot and replays it on
// the next draw. A by-name emulation that swapped the slot behind the binding accounting left the
// replay driving A's backend twin from B's frontend object, so A's storage was respecified with
// B's shape and its GPU-only contents were lost (Iris/BSL: the 16x16 lightmap blanked when a
// 2048x2048 shadow map was uploaded by name, and the HUD text discarded itself). The probe ran only
// at the P2 subsystem mask, whose legacy unit arm is gone; this asserts the same thing through
// public GL, on every lane and on both backends, at the shipping mask.

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

        constexpr int kInset = 2;
        constexpr int kSmall = 4;  // A: the lightmap
        constexpr int kLarge = 32; // B: the shadow map

        constexpr const char* kQuadVS = R"(#version 330 core
in vec2 aPos;
out vec2 vUv;
void main() {
    vUv = aPos * 0.5 + 0.5;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

        // texelFetch at the texture's own last texel: a texture whose shape moved to B's would
        // fetch a different texel, and one whose contents moved would fetch B's colour.
        constexpr const char* kFetchFS = R"(#version 330 core
uniform sampler2D uTex;
out vec4 oColor;
void main() {
    ivec2 size = textureSize(uTex, 0);
    oColor = (size == ivec2(4, 4)) ? texelFetch(uTex, size - ivec2(1, 1), 0) : vec4(1.0, 0.0, 1.0, 1.0);
}
)";

        struct Vertex {
            float x, y;
        };

        std::vector<std::uint8_t> Solid(int size, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
            std::vector<std::uint8_t> texels(static_cast<std::size_t>(size) * size * 4);
            for (std::size_t i = 0; i < texels.size(); i += 4) {
                texels[i] = r;
                texels[i + 1] = g;
                texels[i + 2] = b;
                texels[i + 3] = 255;
            }
            return texels;
        }

        class DsaUpdateKeepsTheBoundUnitScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                std::string error;
                m_program = CompileProgram(kQuadVS, kFetchFS, &error);
                ASSERT_NE(m_program, 0u) << error;
                static const Vertex quad[6] = {{-1.0f, -1.0f}, {1.0f, -1.0f}, {1.0f, 1.0f},
                                               {-1.0f, -1.0f}, {1.0f, 1.0f},  {-1.0f, 1.0f}};
                glGenBuffers(1, &m_quad);
                glBindBuffer(GL_ARRAY_BUFFER, m_quad);
                glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
                glGenVertexArrays(1, &m_vao);
                glBindVertexArray(m_vao);
                glEnableVertexAttribArray(0);
                glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), nullptr);
                glBindVertexArray(0);
                ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "the scene setup left a GL error behind";
            }

            void TearDown() override {
                if (!Ready() || IsSkipped()) return;
                glUseProgram(0);
                glBindVertexArray(0);
                if (m_vao != 0) glDeleteVertexArrays(1, &m_vao);
                if (m_quad != 0) glDeleteBuffers(1, &m_quad);
                if (m_program != 0) glDeleteProgram(m_program);
            }

            // The classic path for A, on purpose: it is BOUND to unit 0 and sampled through the
            // binding, which is what puts it on the per-unit list the hazard lived in.
            GLuint MakeBoundTexture(int size, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
                const auto texels = Solid(size, r, g, b);
                GLuint texture = 0;
                glGenTextures(1, &texture);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, texture);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, size, size, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels.data());
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                return texture;
            }

            // Samples whatever unit 0 holds now - no bind here, the unit is the subject.
            Image DrawSamplingUnitZero() {
                BindDefaultFramebuffer();
                glViewport(0, 0, Gl().Width(), Gl().Height());
                ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
                glUseProgram(m_program);
                glUniform1i(glGetUniformLocation(m_program, "uTex"), 0);
                glBindVertexArray(m_vao);
                glDrawArrays(GL_TRIANGLES, 0, 6);
                const Image image = ReadPixels(Gl().Width(), Gl().Height());
                Gl().EndFrame();
                return image;
            }

            ::testing::AssertionResult WholeViewportIs(const Image& image, const char* expected,
                                                       const std::string& when) {
                return RegionIsMostly(image, kInset, image.Width() - kInset, kInset,
                                      image.Height() - kInset, expected, 0.0, when);
            }

            GLuint m_program = 0;
            GLuint m_quad = 0;
            GLuint m_vao = 0;
        };

        TEST_F(DsaUpdateKeepsTheBoundUnitScenario, AByNameUploadToAnotherTextureLeavesTheUnitsTextureIntact) {
            if (!Ready()) return;
            GLuint probe = 0;
            glCreateTextures(GL_TEXTURE_2D, 1, &probe);
            if (FirstGLError() != GLenum(GL_NO_ERROR) || probe == 0) {
                GTEST_SKIP() << "glCreateTextures is not usable here; the case needs a by-name update";
            }
            glDeleteTextures(1, &probe);

            // B first, then unbound: it exists but holds no unit.
            const GLuint b = MakeBoundTexture(kSmall, 0, 0, 255);
            glBindTexture(GL_TEXTURE_2D, 0);
            // A on unit 0, sampled once so the unit's sync list is built and memoised.
            const GLuint a = MakeBoundTexture(kSmall, 255, 0, 0);
            EXPECT_TRUE(WholeViewportIs(DrawSamplingUnitZero(), "red", "A sampled through unit 0"));

            // THE HAZARD: B respecified to a different shape and filled BY NAME while A stays bound.
            const auto large = Solid(kLarge, 0, 255, 0);
            // (glTextureStorage2D on a mutable texture is legal and makes it immutable at the new shape.)
            glTextureStorage2D(b, 1, GL_RGBA8, kLarge, kLarge);
            glTextureSubImage2D(b, 0, 0, 0, kLarge, kLarge, GL_RGBA, GL_UNSIGNED_BYTE, large.data());
            EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));

            EXPECT_TRUE(WholeViewportIs(DrawSamplingUnitZero(), "red",
                                        "unit 0 after a by-name update of ANOTHER texture: it must still sample A "
                                        "(magenta = A took B's shape, green = A took B's texels)"));

            // And the update itself landed where it was addressed.
            GLint bound = 0;
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
            EXPECT_EQ(static_cast<GLuint>(bound), a) << "a by-name update moved the unit's binding";

            glBindTexture(GL_TEXTURE_2D, 0);
            glDeleteTextures(1, &a);
            glDeleteTextures(1, &b);
        }

    } // namespace
} // namespace MGITest
