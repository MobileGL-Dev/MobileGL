// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/CopyImageLevelRangeScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// KHR-GL43.copy_image.non_existent_mipmap, and what it cost.
//
// The CTS case is a pure negative test: two 16x16 textures that have level 0 and
// nothing else, and a glCopyImageSubData naming level 1. The answer is
// GL_INVALID_VALUE (GL 4.6 core 18.3.2 / ARB_copy_image: "srcLevel/dstLevel is not
// a valid level"). MobileGL's frontend only checked the level against
// GL_MAX_TEXTURE_SIZE, so level 1 sailed through into the backends, DirectVulkan
// resolved it into a VkImageCopy subresource on a VkImage that was created with
// exactly one mip level, and the Adreno driver dereferenced the level it was
// promised - SIGSEGV inside vkCmdCopyImage, taking the whole glcts process down
// mid-run. A negative case must never do that.
//
// So the level-1-on-a-one-level-texture rejection is the regression proper, and the
// rest of this file is what keeps the fix honest. A validator that answered
// GL_INVALID_VALUE to every level would satisfy the regression tests alone, so the
// scenarios below pin the BOUNDARY rather than the symptom:
//
//   * a texture that really does have two levels must accept a copy at level 1,
//   * the same texture must still reject level 2,
//   * and a plain level-0 copy must move pixels, which is checked by reading the
//     destination back rather than by trusting glGetError.
//
// Both backends are covered because the fix is in the shared frontend: DirectGLES
// forwards to the ES glCopyImageSubData (whose own error lands in the ES context,
// not in MobileGL's, so it never reached the application either) and DirectVulkan
// records the copy itself.

#include <array>
#include <cstring>
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

        constexpr GLsizei kSize = 16;

        struct Rgba8 {
            GLubyte r, g, b, a;
            bool operator==(const Rgba8& other) const {
                return r == other.r && g == other.g && b == other.b && a == other.a;
            }
        };

        std::vector<Rgba8> SolidImage(GLsizei width, GLsizei height, Rgba8 color) {
            return std::vector<Rgba8>(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), color);
        }

        class CopyImageLevelRangeScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                DrainErrors();
            }

            void TearDown() override {
                if (!Ready()) return;
                DeleteTextures();
                if (m_fbo != 0) {
                    glBindFramebuffer(GL_FRAMEBUFFER, 0);
                    glDeleteFramebuffers(1, &m_fbo);
                    m_fbo = 0;
                }
                DrainErrors();
                ScenarioTest::TearDown();
            }

            static void DrainErrors() {
                for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; ++i) {
                }
            }

            void DeleteTextures() {
                if (m_src != 0) glDeleteTextures(1, &m_src);
                if (m_dst != 0) glDeleteTextures(1, &m_dst);
                m_src = 0;
                m_dst = 0;
            }

            // One 16x16 RGBA8 texture with `levelCount` levels defined through
            // glTexImage2D - the same way the CTS case builds its textures, and
            // deliberately NOT glTexStorage2D: an immutable allocation would define the
            // whole chain up front and could not express "level 1 does not exist".
            GLuint MakeTexture(int levelCount, Rgba8 baseColor) {
                GLuint texture = 0;
                glGenTextures(1, &texture);
                glBindTexture(GL_TEXTURE_2D, texture);
                for (int level = 0; level < levelCount; ++level) {
                    const GLsizei extent = kSize >> level;
                    const std::vector<Rgba8> pixels = SolidImage(extent, extent, baseColor);
                    glTexImage2D(GL_TEXTURE_2D, level, GL_RGBA8, extent, extent, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                                 pixels.data());
                }
                // What Utils::makeTextureComplete does in the CTS case: the texture is
                // complete for the levels it actually has, not for a chain it does not.
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, levelCount - 1);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glBindTexture(GL_TEXTURE_2D, 0);
                return texture;
            }

            void MakePair(int levelCount) {
                DeleteTextures();
                m_src = MakeTexture(levelCount, Rgba8{11, 22, 33, 255});
                m_dst = MakeTexture(levelCount, Rgba8{200, 100, 50, 255});
                ASSERT_EQ(glGetError(), GL_NO_ERROR) << "texture setup with " << levelCount << " level(s)";
            }

            // The call under test, at whatever levels the caller wants, over a 1x1
            // region so the region check can never be what rejects it.
            GLenum CopyAt(GLint srcLevel, GLint dstLevel, GLsizei extent = 1) {
                DrainErrors();
                glCopyImageSubData(m_src, GL_TEXTURE_2D, srcLevel, 0, 0, 0, m_dst, GL_TEXTURE_2D, dstLevel, 0, 0, 0,
                                   extent, extent, 1);
                const GLenum error = glGetError();
                // A second pending error would mean the entry point queued more than one,
                // and the extra would be handed out at an unrelated call site later.
                EXPECT_EQ(glGetError(), GL_NO_ERROR) << "the copy recorded more than one error";
                return error;
            }

            Rgba8 ReadBackDestinationLevel0() {
                if (m_fbo == 0) glGenFramebuffers(1, &m_fbo);
                glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_dst, 0);
                const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
                if (status != GL_FRAMEBUFFER_COMPLETE) {
                    ADD_FAILURE() << "readback framebuffer incomplete: " << status;
                    glBindFramebuffer(GL_FRAMEBUFFER, 0);
                    return Rgba8{0, 0, 0, 0};
                }
                Rgba8 texel{0, 0, 0, 0};
                glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &texel);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                return texel;
            }

            GLuint m_src = 0;
            GLuint m_dst = 0;
            GLuint m_fbo = 0;
        };

        // The regression. Level 1 of a texture that has only level 0 is not a level, and
        // saying so is the whole job: before the fix this reached DirectVulkan, which
        // handed mipLevel=1 to vkCmdCopyImage on a one-level VkImage and died inside the
        // Adreno driver.
        TEST_F(CopyImageLevelRangeScenario, LevelOneOfASingleLevelTextureIsRejected) {
            if (!Ready()) GTEST_SKIP();
            MakePair(1);

            EXPECT_EQ(CopyAt(1, 0), static_cast<GLenum>(GL_INVALID_VALUE)) << "source level 1";
            EXPECT_EQ(CopyAt(0, 1), static_cast<GLenum>(GL_INVALID_VALUE)) << "destination level 1";
            EXPECT_EQ(CopyAt(1, 1), static_cast<GLenum>(GL_INVALID_VALUE)) << "both levels 1";
        }

        // The negative control that makes the test above falsifiable: the same level
        // index, on textures that genuinely have it, must be accepted. A validator that
        // rejected every non-zero level would pass the regression test and fail here.
        TEST_F(CopyImageLevelRangeScenario, LevelOneOfATwoLevelTextureIsAccepted) {
            if (!Ready()) GTEST_SKIP();
            MakePair(2);

            EXPECT_EQ(CopyAt(1, 1), static_cast<GLenum>(GL_NO_ERROR));
        }

        // And the boundary from the other side: two levels means 0 and 1, not 2.
        TEST_F(CopyImageLevelRangeScenario, LevelTwoOfATwoLevelTextureIsRejected) {
            if (!Ready()) GTEST_SKIP();
            MakePair(2);

            EXPECT_EQ(CopyAt(2, 0), static_cast<GLenum>(GL_INVALID_VALUE)) << "source level 2";
            EXPECT_EQ(CopyAt(0, 2), static_cast<GLenum>(GL_INVALID_VALUE)) << "destination level 2";
        }

        // Errors alone cannot tell an accepted copy from a silently dropped one, so the
        // ordinary case is checked by reading the destination back: the copy has to move
        // the source's texel, not merely decline to complain.
        TEST_F(CopyImageLevelRangeScenario, AValidLevelZeroCopyStillMovesPixels) {
            if (!Ready()) GTEST_SKIP();
            MakePair(1);

            ASSERT_EQ(ReadBackDestinationLevel0(), (Rgba8{200, 100, 50, 255})) << "destination before the copy";
            EXPECT_EQ(CopyAt(0, 0, kSize), static_cast<GLenum>(GL_NO_ERROR));
            EXPECT_EQ(ReadBackDestinationLevel0(), (Rgba8{11, 22, 33, 255})) << "destination after the copy";
        }

    } // namespace
} // namespace MGITest
