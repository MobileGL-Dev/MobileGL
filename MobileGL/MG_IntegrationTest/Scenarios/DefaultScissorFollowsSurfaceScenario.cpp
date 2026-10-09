// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/DefaultScissorFollowsSurfaceScenario.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - A SCISSOR BOX THE APPLICATION NEVER WROTE FOLLOWS THE CURRENT DRAW SURFACE.
//
// Until the first glScissor, DirectGLES substitutes the current draw surface's size for the
// scissor box (the Minecraft protection described at RenderStateImpl::SyncRenderState), and it
// keeps the surface size in a memo that a swap, a change of draw surface, or a surface's creation
// or destruction invalidates. This case makes a second, larger surface current on the same
// context and clears it with the scissor test on: the clear must reach the larger surface's far
// corner, which a size remembered from the first surface would cut off.

#include <array>

#include "../Harness/ScenarioFixture.h"

#ifdef GLAPI
#undef GLAPI
#endif
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glcorearb.h>
#undef GL_GLEXT_PROTOTYPES

#include <EGL/egl.h>

namespace MGITest {
namespace {

class DefaultScissorFollowsSurfaceScenario : public ScenarioTest {};

TEST_F(DefaultScissorFollowsSurfaceScenario, AClearOnALargerSurfaceMadeCurrentReachesItsFarCorner) {
    if (!Ready() || IsSkipped()) return;
    if (Gl().BackendName() != "DirectGLES") GTEST_SKIP() << "the substituted scissor box is DirectGLES's";
    const EGLDisplay display = eglGetCurrentDisplay();
    const EGLSurface first = eglGetCurrentSurface(EGL_DRAW);
    const EGLContext context = eglGetCurrentContext();
    ASSERT_NE(display, EGL_NO_DISPLAY);
    ASSERT_NE(first, EGL_NO_SURFACE);
    EGLint width = 0, height = 0, configId = 0, configs = 0;
    ASSERT_TRUE(eglQuerySurface(display, first, EGL_WIDTH, &width));
    ASSERT_TRUE(eglQuerySurface(display, first, EGL_HEIGHT, &height));
    ASSERT_TRUE(eglQueryContext(display, context, EGL_CONFIG_ID, &configId));
    const EGLint pick[] = {EGL_CONFIG_ID, configId, EGL_NONE};
    EGLConfig config = nullptr;
    ASSERT_TRUE(eglChooseConfig(display, pick, &config, 1, &configs));
    ASSERT_EQ(configs, 1);

    // The first surface's size goes into the memo: a clear with the scissor test on and no box.
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_CULL_FACE);
    glEnable(GL_SCISSOR_TEST);
    glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();

    const EGLint attribs[] = {EGL_WIDTH, width * 2, EGL_HEIGHT, height * 2, EGL_NONE};
    const EGLSurface second = eglCreatePbufferSurface(display, config, attribs);
    if (second == EGL_NO_SURFACE) GTEST_SKIP() << "no second pbuffer surface on this platform";
    ASSERT_TRUE(eglMakeCurrent(display, second, second, context));
    glViewport(0, 0, width * 2, height * 2);
    glEnable(GL_CULL_FACE); // a capability change, so the render state is synced again for the clear
    glClearColor(0.0f, 1.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    std::array<GLubyte, 4> corner{};
    glReadPixels(width * 2 - 1, height * 2 - 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, corner.data());
    EXPECT_EQ(int(corner[0]), 0) << "the clear reaches the larger surface's far corner";
    EXPECT_EQ(int(corner[1]), 255) << "the clear reaches the larger surface's far corner";

    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    ASSERT_TRUE(eglMakeCurrent(display, first, first, context));
    glViewport(0, 0, width, height);
    eglDestroySurface(display, second);
    EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
}

} // namespace
} // namespace MGITest
