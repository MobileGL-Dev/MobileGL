// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/DepthStencilReadbackScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - glReadPixels OF DEPTH AND STENCIL FROM THE DEFAULT FRAMEBUFFER.
//
// DirectVulkan's depth/stencil readback used to decline the default framebuffer outright
// (`ReadDepthStencilPixels` returned at its first line) because that framebuffer's depth and
// stencil "attachments" are placeholder texture objects backing no image - the real one is the
// swapchain's depth/stencil twin. Declining meant the call raised no GL error and wrote NOTHING,
// so the caller kept whatever its buffer already held.
//
// That silence is what the framebuffer_blit family trips over. Every one of its cases begins by
// clearing the default framebuffer's depth and stencil and reading them straight back as a
// sanity check, into a local pre-initialised to 0.2 (depth) and 50 (stencil); an untouched
// buffer therefore reports "expected DEPTH[0.25] but got DEPTH[0.2]" and "expected STENCIL[1] but
// got STENCIL[50]" - the exact strings in the 15 Magma failures - long before any blit happens.
// A test that only checked "no GL error" would pass against the broken path, so every case here
// poisons its destination with a value the correct answer cannot be.
//
// The orientation case is the second half. This renderer stores the default framebuffer
// display-side-up and converts GL rects on their way in, so the depth copy needs the same rect
// mapping and row re-ordering the colour readback got in the M-1 fix; without them a
// vertically-varying depth buffer reads back mirrored, which no full-extent uniform-value test
// can see.
//
// Depth/stencil readback through a USER framebuffer already worked and is asserted here too, as
// the built-in control: it shares ReadDepthStencilImageToClient with the default-framebuffer
// path, so it is what says a failure is about the default framebuffer specifically.

#include <cmath>
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

        // Values no correct read can produce, so "the backend wrote nothing" fails loudly instead
        // of passing on whatever happened to be in the variable. These are the CTS's own poison
        // values, which is why its logs report exactly them.
        constexpr float kDepthPoison = 0.2f;
        constexpr int kStencilPoison = 50;

        class DepthStencilReadbackScenario : public ScenarioTest {
        protected:
            // DirectGLES reads depth and stencil back through the ES driver, which has no
            // guaranteed path for either (GL_NV_read_depth / GL_NV_read_stencil are optional and
            // absent on both the Adreno device and Mesa's ES). That gap is tracked separately as
            // the packed_depth_stencil cluster and needs a shader-sampling emulation, not this
            // change; asserting it here would only pin a known-missing feature.
            bool BackendReadsDepthStencil() const { return Gl().BackendName() == "DirectVulkan"; }

            float ReadDepthAt(int x, int y) const {
                float depth = kDepthPoison;
                glReadPixels(x, y, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &depth);
                return depth;
            }

            int ReadStencilAt(int x, int y) const {
                int stencil = kStencilPoison;
                glReadPixels(x, y, 1, 1, GL_STENCIL_INDEX, GL_INT, &stencil);
                return stencil;
            }
        };

        // A depth buffer whose value depends on the row: bottom half `bottom`, top half `top`.
        // Built with a scissored clear rather than a draw so the test stays independent of
        // depth-test and shader behaviour.
        void ClearDepthInBands(int width, int height, float bottom, float top) {
            glEnable(GL_SCISSOR_TEST);
            glScissor(0, 0, width, height / 2);
            glClearDepth(bottom);
            glClear(GL_DEPTH_BUFFER_BIT);
            glScissor(0, height / 2, width, height - height / 2);
            glClearDepth(top);
            glClear(GL_DEPTH_BUFFER_BIT);
            glDisable(GL_SCISSOR_TEST);
        }

    } // namespace

    TEST_F(DepthStencilReadbackScenario, DefaultFramebufferDepthClearIsVisibleToReadPixels) {
        if (!Ready()) return;
        if (!BackendReadsDepthStencil()) {
            GTEST_SKIP() << "backend " << Gl().BackendName()
                         << " has no depth readback path (ES lacks GL_NV_read_depth); see the packed_depth_stencil "
                            "cluster";
        }
        HeadlessGL& gl = Gl();
        const int width = gl.Width();
        const int height = gl.Height();

        BindDefaultFramebuffer();
        glViewport(0, 0, width, height);
        glDisable(GL_SCISSOR_TEST);
        glDepthMask(GL_TRUE);
        glClearDepth(0.25);
        glClear(GL_DEPTH_BUFFER_BIT);

        const float centre = ReadDepthAt(width / 2, height / 2);
        EXPECT_EQ(FirstGLError(), 0u);
        EXPECT_NEAR(centre, 0.25f, 1.0f / 4096.0f)
            << "glReadPixels(GL_DEPTH_COMPONENT) of the default framebuffer returned " << centre
            << (std::fabs(centre - kDepthPoison) < 1e-6f ? " - the destination was never written at all" : "");

        gl.EndFrame();
    }

    TEST_F(DepthStencilReadbackScenario, DefaultFramebufferStencilClearIsVisibleToReadPixels) {
        if (!Ready()) return;
        if (!BackendReadsDepthStencil()) {
            GTEST_SKIP() << "backend " << Gl().BackendName()
                         << " has no stencil readback path (ES lacks GL_NV_read_stencil); see the "
                            "packed_depth_stencil cluster";
        }
        HeadlessGL& gl = Gl();
        const int width = gl.Width();
        const int height = gl.Height();

        BindDefaultFramebuffer();
        glViewport(0, 0, width, height);
        glDisable(GL_SCISSOR_TEST);
        glStencilMask(0xFFu);
        glClearStencil(3);
        glClear(GL_STENCIL_BUFFER_BIT);

        const int centre = ReadStencilAt(width / 2, height / 2);
        EXPECT_EQ(FirstGLError(), 0u);
        EXPECT_EQ(centre, 3) << "glReadPixels(GL_STENCIL_INDEX) of the default framebuffer returned " << centre
                             << (centre == kStencilPoison ? " - the destination was never written at all" : "");

        gl.EndFrame();
    }

    // The orientation half: a depth buffer that varies with the row must read back in GL's
    // bottom-up order. A full-extent uniform clear is a fixed point of the flip, so only a banded
    // buffer can tell the two apart.
    TEST_F(DepthStencilReadbackScenario, DefaultFramebufferDepthReadbackKeepsTheGLRowOrder) {
        if (!Ready()) return;
        if (!BackendReadsDepthStencil()) {
            GTEST_SKIP() << "backend " << Gl().BackendName() << " has no depth readback path";
        }
        HeadlessGL& gl = Gl();
        const int width = gl.Width();
        const int height = gl.Height();
        ASSERT_GE(height, 8);

        BindDefaultFramebuffer();
        glViewport(0, 0, width, height);
        glDepthMask(GL_TRUE);
        ClearDepthInBands(width, height, /*bottom=*/0.25f, /*top=*/0.75f);
        EXPECT_EQ(FirstGLError(), 0u);

        const float bottom = ReadDepthAt(width / 2, height / 4);
        const float top = ReadDepthAt(width / 2, height - 1 - height / 4);
        EXPECT_EQ(FirstGLError(), 0u);
        EXPECT_NEAR(bottom, 0.25f, 1.0f / 4096.0f)
            << "GL row " << (height / 4) << " is in the bottom band and was cleared to 0.25, but read back " << bottom
            << " (0.75 there means the readback is upside down)";
        EXPECT_NEAR(top, 0.75f, 1.0f / 4096.0f)
            << "GL row " << (height - 1 - height / 4) << " is in the top band and was cleared to 0.75, but read back "
            << top << " (0.25 there means the readback is upside down)";

        gl.EndFrame();
    }

    // The control: the same read against a user framebuffer, which never went through the
    // declined path. It is what makes a failure above specific to the default framebuffer.
    TEST_F(DepthStencilReadbackScenario, UserFramebufferDepthClearIsVisibleToReadPixels) {
        if (!Ready()) return;
        if (!BackendReadsDepthStencil()) {
            GTEST_SKIP() << "backend " << Gl().BackendName() << " has no depth readback path";
        }
        HeadlessGL& gl = Gl();
        const int width = 64;
        const int height = 48;

        GLuint fbo = 0, colorTex = 0, depthTex = 0;
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glGenTextures(1, &colorTex);
        glBindTexture(GL_TEXTURE_2D, colorTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTex, 0);
        glGenTextures(1, &depthTex);
        glBindTexture(GL_TEXTURE_2D, depthTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, width, height, 0, GL_DEPTH_STENCIL,
                     GL_UNSIGNED_INT_24_8, nullptr);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, depthTex, 0);
        ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GLenum(GL_FRAMEBUFFER_COMPLETE));
        ASSERT_EQ(FirstGLError(), 0u);

        glViewport(0, 0, width, height);
        glDisable(GL_SCISSOR_TEST);
        glDepthMask(GL_TRUE);
        glStencilMask(0xFFu);
        glClearDepth(0.5);
        glClearStencil(7);
        glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

        const float depth = ReadDepthAt(width / 2, height / 2);
        const int stencil = ReadStencilAt(width / 2, height / 2);
        EXPECT_EQ(FirstGLError(), 0u);
        EXPECT_NEAR(depth, 0.5f, 1.0f / 4096.0f) << "user-framebuffer depth readback returned " << depth;
        EXPECT_EQ(stencil, 7) << "user-framebuffer stencil readback returned " << stencil;

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteTextures(1, &depthTex);
        glDeleteTextures(1, &colorTex);
        glDeleteFramebuffers(1, &fbo);
        gl.EndFrame();
    }
} // namespace MGITest
