// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/EmptyTextureLevelScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - A TEXTURE LEVEL DEFINED EMPTY IS LEGAL GL, AND IT MUST NOT END THE SESSION.
//
// glTexImage1D(width 0), glTexImage2D(0 x 0), glTexImage3D(0 x 0 x 0) define a level with no
// texels. dEQP's glu::resetStateGLCore issues exactly that for EVERY texture target on EVERY unit
// after EVERY case (gluStateReset.cpp), so a split arm that cannot carry an empty level cannot run
// a single KHR-GL case to completion.
//
// P7 PH-4 (f90afcd1) put each per-level respecify's exact extent on the wire and made the server
// refuse any zero component as a noncanonical carrier - Fatal{ProtocolCorruption,
// "ResourceRespecify.ExtentCarrier"} on the apply thread, from the reset that follows the case
// rather than the case itself. The two cases below are the reset's own sequence, then the
// round trip that proves a level can go empty and come back with texels the server really staged.

#include <array>
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

        class EmptyTextureLevelScenario : public ScenarioTest {
        protected:
            void TearDown() override {
                if (!Ready()) return;
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                if (m_fbo != 0) glDeleteFramebuffers(1, &m_fbo);
                if (m_texture != 0) glDeleteTextures(1, &m_texture);
                m_fbo = m_texture = 0;
                while (glGetError() != GL_NO_ERROR) {
                }
            }

            // Every target resetStateGLCore empties, in its order, on the default textures.
            static void EmptyEveryDefaultTarget() {
                glBindTexture(GL_TEXTURE_1D, 0);
                glTexImage1D(GL_TEXTURE_1D, 0, GL_RGBA, 0, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                glBindTexture(GL_TEXTURE_2D, 0);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 0, 0, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
                for (GLenum face = GL_TEXTURE_CUBE_MAP_POSITIVE_X; face <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z; ++face)
                    glTexImage2D(face, 0, GL_RGBA, 0, 0, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                glBindTexture(GL_TEXTURE_1D_ARRAY, 0);
                glTexImage2D(GL_TEXTURE_1D_ARRAY, 0, GL_RGBA, 0, 0, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
                glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA, 0, 0, 0, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                glBindTexture(GL_TEXTURE_3D, 0);
                glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA, 0, 0, 0, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                glBindTexture(GL_TEXTURE_RECTANGLE, 0);
                glTexImage2D(GL_TEXTURE_RECTANGLE, 0, GL_RGBA, 0, 0, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            }

            GLuint m_texture = 0;
            GLuint m_fbo = 0;
        };

    } // namespace

    // dEQP's reset, verbatim in shape: every default texture target defined EMPTY at level 0,
    // twice (the reset runs after every case), then an ordinary clear + readback. Before the fix
    // the first glTexImage1D ended the session on every split arm; the readback is what proves
    // the session is still the one the case started with.
    TEST_F(EmptyTextureLevelScenario, TheCtsStateResetDefinesEveryDefaultTargetEmpty) {
        if (!Ready()) return;
        for (int pass = 0; pass < 2; ++pass) {
            glActiveTexture(GL_TEXTURE0);
            EmptyEveryDefaultTarget();
            glActiveTexture(GL_TEXTURE1);
            EmptyEveryDefaultTarget();
            glActiveTexture(GL_TEXTURE0);
            ASSERT_EQ(FirstGLError(), 0u) << "an empty level definition is legal GL (pass " << pass << ")";
        }

        HeadlessGL& gl = Gl();
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, gl.Width(), gl.Height());
        glDisable(GL_SCISSOR_TEST);
        glClearColor(0.0f, 1.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        std::array<GLubyte, 4> pixel{};
        glReadPixels(1, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
        EXPECT_EQ(FirstGLError(), 0u);
        // The default framebuffer's readback is not what this case is about (it reads zeros on some
        // DirectVulkan hosts, tools/cts/README.md) - what matters is that the call RETURNED, i.e.
        // the session that took the empty levels is still answering.
        gl.EndFrame();
    }

    // A NAMED texture's level goes 4x4 (with texels) -> EMPTY -> 2x2 (with other texels), and the
    // 2x2 content must be what a framebuffer readback of that level returns. This is the staged
    // store's half: the empty definition must drop the 4x4 bytes and coverage (the coordinate
    // system was replaced), and must not stand in the way of the next non-empty definition
    // adopting its own bytes.
    TEST_F(EmptyTextureLevelScenario, ALevelDefinedEmptyThenRedefinedCarriesTheNewTexels) {
        if (!Ready()) return;
        glGenTextures(1, &m_texture);
        glBindTexture(GL_TEXTURE_2D, m_texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        std::vector<GLubyte> redTexels(4 * 4 * 4, 0);
        for (std::size_t i = 0; i < redTexels.size(); i += 4) {
            redTexels[i] = 255;
            redTexels[i + 3] = 255;
        }
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, redTexels.data());
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 0, 0, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr); // empty
        std::vector<GLubyte> greenTexels(2 * 2 * 4, 0);
        for (std::size_t i = 0; i < greenTexels.size(); i += 4) {
            greenTexels[i + 1] = 255;
            greenTexels[i + 3] = 255;
        }
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, greenTexels.data());
        ASSERT_EQ(FirstGLError(), 0u) << "4x4 -> empty -> 2x2 on one level is legal GL";

        glGenFramebuffers(1, &m_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_texture, 0);
        ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GLenum(GL_FRAMEBUFFER_COMPLETE));
        std::vector<GLubyte> readBack(2 * 2 * 4, 7);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, readBack.data());
        EXPECT_EQ(FirstGLError(), 0u);
        EXPECT_EQ(readBack, greenTexels)
            << "the level that went empty and came back must hold the 2x2 texels it was redefined with";
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        Gl().EndFrame();
    }

} // namespace MGITest
