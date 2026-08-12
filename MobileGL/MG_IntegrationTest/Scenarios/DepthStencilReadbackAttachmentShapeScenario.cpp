// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/DepthStencilReadbackAttachmentShapeScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - DEPTH/STENCIL READBACK WHEN THE ATTACHMENT IS NOT A PLAIN GL_TEXTURE_2D,
// AND THE DEFAULT FRAMEBUFFER'S ADVERTISED DEPTH/STENCIL FORMAT.
//
// Three shipped defects, all of them invisible to a test that only ever attaches a 2D texture
// or only ever asks the default framebuffer for a colour value.
//
// (1) The ES depth/stencil readback emulation identifies the source format by binding the
//     attachment's texture NAME to GL_TEXTURE_2D and asking that target for its internal
//     format. A name whose target is GL_TEXTURE_2D_ARRAY (attached by
//     glFramebufferTextureLayer) makes the bind answer GL_INVALID_OPERATION and change
//     nothing - so the query then truthfully describes whatever texture was already on
//     GL_TEXTURE_2D, which on that path is the emulation's own staging scratch. A wrong
//     answer that looks like a right one: the staging blit is issued between mismatched
//     depth formats, ES rejects it, and the read reports nothing at all.
//
// (2) Adreno answers GL_NONE for GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE on an attachment made
//     by glFramebufferTexture (a cube map, attached layered) while still reporting its depth
//     and stencil bits correctly. The emulation took OBJECT_TYPE as the sole witness for "is
//     there an aspect here at all" and declined the whole read.
//
// (3) DirectGLES never told the frontend what its default framebuffer's depth/stencil format
//     actually is, so the placeholder from MG_Impl/Init.cpp - GL_DEPTH32F_STENCIL8 - was what
//     every attachment query answered, whatever the surface really had. That is not cosmetic:
//     GL blits depth/stencil only between IDENTICAL formats, so an application that reads
//     GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE, allocates the buffer it was just told about and
//     blits gets GL_INVALID_OPERATION - and a rejected glBlitFramebuffer transfers NOTHING,
//     colour bits included. DirectVulkan has published its real format since the swapchain
//     work; this is the half that was missing.
//
// Every case poisons its destination with a value the correct answer cannot be, so "the
// backend wrote nothing" fails loudly instead of passing on stale memory. The plain
// GL_TEXTURE_2D case at the end is the built-in control: it shares every line of the readback
// path with the array and cube cases, so its passing is what says a failure above is about the
// attachment's SHAPE and not about depth readback in general.
//
// The scenario name starts with DepthStencilReadback on purpose - that is the filter the
// forced-emulation ctest registration uses (MG_IntegrationTest/CMakeLists.txt), and without
// that registration these cases are unfalsifiable on llvmpipe, which accepts the native ES
// depth reads that the Adreno device does not have.

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

        constexpr float kDepthPoison = 0.2f;
        constexpr int kStencilPoison = 50;
        constexpr float kDepthValue = 0.75f;
        constexpr int kStencilValue = 7;
        constexpr int kSize = 16;

        class DepthStencilReadbackAttachmentShapeScenario : public ScenarioTest {
        protected:
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

            // Clears the currently bound framebuffer's depth and stencil to the shared
            // reference values, with both write masks explicitly open (glClear honours them,
            // and a leftover mask from another scenario in this shared context would look
            // exactly like the bug under test).
            void ClearDepthStencil() const {
                glDepthMask(GL_TRUE);
                glStencilMask(0xFFu);
                glDisable(GL_SCISSOR_TEST);
                glClearDepth(kDepthValue);
                glClearStencil(kStencilValue);
                glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
            }
        };

        // Fails the calling test if the framebuffer bound at both targets is not complete;
        // an incomplete framebuffer would make every read below return the poison for a
        // reason that has nothing to do with what is being tested.
        ::testing::AssertionResult FramebufferIsComplete() {
            const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            if (status == GL_FRAMEBUFFER_COMPLETE) return ::testing::AssertionSuccess();
            return ::testing::AssertionFailure() << "framebuffer status 0x" << std::hex << status;
        }

    } // namespace

    // (1) A depth slice of a 2D ARRAY texture, attached with glFramebufferTextureLayer.
    // Pre-fix this read back the poison: the format probe answered with the staging scratch's
    // GL_DEPTH24_STENCIL8 instead of the array's GL_DEPTH_COMPONENT24, and the mismatched
    // staging blit was rejected.
    TEST_F(DepthStencilReadbackAttachmentShapeScenario, DepthOfAnArrayLayerAttachmentReadsBack) {
        if (!Ready()) return;
        HeadlessGL& gl = Gl();

        GLuint fbo = 0;
        GLuint depthArray = 0;
        glGenFramebuffers(1, &fbo);
        glGenTextures(1, &depthArray);
        glBindTexture(GL_TEXTURE_2D_ARRAY, depthArray);
        glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_DEPTH_COMPONENT24, kSize, kSize, 4);
        glBindTexture(GL_TEXTURE_2D_ARRAY, 0);

        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        // Layer 2, not layer 0: a backend that silently reads the wrong slice would still
        // agree with a single-layer texture.
        glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, depthArray, 0, 2);
        glDrawBuffer(GL_NONE);
        glReadBuffer(GL_NONE);
        EXPECT_EQ(FirstGLError(), 0u);
        ASSERT_TRUE(FramebufferIsComplete());

        glViewport(0, 0, kSize, kSize);
        ClearDepthStencil();

        const float depth = ReadDepthAt(kSize / 2, kSize / 2);
        EXPECT_EQ(FirstGLError(), 0u);
        EXPECT_NEAR(depth, kDepthValue, 1.0f / 4096.0f)
            << "glReadPixels(GL_DEPTH_COMPONENT) of a GL_TEXTURE_2D_ARRAY layer attachment returned " << depth
            << (std::fabs(depth - kDepthPoison) < 1e-6f ? " - the destination was never written at all" : "");

        BindDefaultFramebuffer();
        glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &depthArray);
        gl.EndFrame();
    }

    // (2) A depth cube map, attached whole with glFramebufferTexture - a LAYERED attachment.
    // Pre-fix the emulation declined outright, because the driver reports GL_NONE for that
    // attachment's OBJECT_TYPE.
    TEST_F(DepthStencilReadbackAttachmentShapeScenario, DepthOfALayeredCubeAttachmentReadsBack) {
        if (!Ready()) return;
        HeadlessGL& gl = Gl();

        GLuint fbo = 0;
        GLuint depthCube = 0;
        glGenFramebuffers(1, &fbo);
        glGenTextures(1, &depthCube);
        glBindTexture(GL_TEXTURE_CUBE_MAP, depthCube);
        glTexStorage2D(GL_TEXTURE_CUBE_MAP, 1, GL_DEPTH_COMPONENT24, kSize, kSize);
        glBindTexture(GL_TEXTURE_CUBE_MAP, 0);

        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, depthCube, 0);
        glDrawBuffer(GL_NONE);
        glReadBuffer(GL_NONE);
        EXPECT_EQ(FirstGLError(), 0u);
        ASSERT_TRUE(FramebufferIsComplete());

        glViewport(0, 0, kSize, kSize);
        ClearDepthStencil();

        const float depth = ReadDepthAt(kSize / 2, kSize / 2);
        EXPECT_EQ(FirstGLError(), 0u);
        EXPECT_NEAR(depth, kDepthValue, 1.0f / 4096.0f)
            << "glReadPixels(GL_DEPTH_COMPONENT) of a layered GL_TEXTURE_CUBE_MAP attachment returned " << depth
            << (std::fabs(depth - kDepthPoison) < 1e-6f ? " - the destination was never written at all" : "");

        BindDefaultFramebuffer();
        glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &depthCube);
        gl.EndFrame();
    }

    // Both aspects of a packed array attachment. The stencil half goes through a different
    // sampling mode than the depth half, and only the depth half was covered above.
    TEST_F(DepthStencilReadbackAttachmentShapeScenario, PackedArrayLayerAttachmentReadsBackBothAspects) {
        if (!Ready()) return;
        HeadlessGL& gl = Gl();

        GLuint fbo = 0;
        GLuint packedArray = 0;
        glGenFramebuffers(1, &fbo);
        glGenTextures(1, &packedArray);
        glBindTexture(GL_TEXTURE_2D_ARRAY, packedArray);
        glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_DEPTH24_STENCIL8, kSize, kSize, 3);
        glBindTexture(GL_TEXTURE_2D_ARRAY, 0);

        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, packedArray, 0, 1);
        glDrawBuffer(GL_NONE);
        glReadBuffer(GL_NONE);
        EXPECT_EQ(FirstGLError(), 0u);
        ASSERT_TRUE(FramebufferIsComplete());

        glViewport(0, 0, kSize, kSize);
        ClearDepthStencil();

        const float depth = ReadDepthAt(kSize / 2, kSize / 2);
        const int stencil = ReadStencilAt(kSize / 2, kSize / 2);
        EXPECT_EQ(FirstGLError(), 0u);
        EXPECT_NEAR(depth, kDepthValue, 1.0f / 4096.0f)
            << "depth of a packed GL_TEXTURE_2D_ARRAY layer attachment returned " << depth;
        EXPECT_EQ(stencil, kStencilValue)
            << "stencil of a packed GL_TEXTURE_2D_ARRAY layer attachment returned " << stencil
            << (stencil == kStencilPoison ? " - the destination was never written at all" : "");

        BindDefaultFramebuffer();
        glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &packedArray);
        gl.EndFrame();
    }

    // The control: the plain GL_TEXTURE_2D shape, which always worked. If this one ever fails
    // alongside the three above, the fault is in depth readback generally rather than in how
    // the attachment's format and presence are discovered.
    TEST_F(DepthStencilReadbackAttachmentShapeScenario, DepthOfAPlainTexture2DAttachmentReadsBack) {
        if (!Ready()) return;
        HeadlessGL& gl = Gl();

        GLuint fbo = 0;
        GLuint depthTex = 0;
        glGenFramebuffers(1, &fbo);
        glGenTextures(1, &depthTex);
        glBindTexture(GL_TEXTURE_2D, depthTex);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_DEPTH_COMPONENT24, kSize, kSize);
        glBindTexture(GL_TEXTURE_2D, 0);

        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depthTex, 0);
        glDrawBuffer(GL_NONE);
        glReadBuffer(GL_NONE);
        EXPECT_EQ(FirstGLError(), 0u);
        ASSERT_TRUE(FramebufferIsComplete());

        glViewport(0, 0, kSize, kSize);
        ClearDepthStencil();

        const float depth = ReadDepthAt(kSize / 2, kSize / 2);
        EXPECT_EQ(FirstGLError(), 0u);
        EXPECT_NEAR(depth, kDepthValue, 1.0f / 4096.0f)
            << "the control case failed: even a plain GL_TEXTURE_2D depth attachment read back " << depth;

        BindDefaultFramebuffer();
        glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &depthTex);
        gl.EndFrame();
    }

    // (3) The default framebuffer must describe its depth/stencil truthfully enough that a
    // buffer allocated from that description is blit-compatible with it. This is the exact
    // sequence KHR-GLxx.framebuffer_blit performs, and the exact reason 22 of its cases died
    // on DirectGLES: the frontend answered 32-bit float depth for a 24-bit fixed-point
    // surface, so the renderbuffer the caller allocated could never be blitted to.
    TEST_F(DepthStencilReadbackAttachmentShapeScenario, DefaultFramebufferDepthStencilFormatIsBlitCompatible) {
        if (!Ready()) return;
        HeadlessGL& gl = Gl();
        const int width = gl.Width();
        const int height = gl.Height();

        BindDefaultFramebuffer();
        GLint depthBits = 0;
        GLint stencilBits = 0;
        GLint componentType = GL_UNSIGNED_NORMALIZED;
        glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_DEPTH,
                                              GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE, &depthBits);
        glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_STENCIL,
                                              GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE, &stencilBits);
        glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_DEPTH,
                                              GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE, &componentType);
        EXPECT_EQ(FirstGLError(), 0u);
        if (depthBits <= 0 || stencilBits <= 0) {
            GTEST_SKIP() << "this surface has no packed depth/stencil (depth=" << depthBits
                         << " stencil=" << stencilBits << "); the blit-compatibility contract needs both";
        }

        // The one sized format the reported description names. Getting here with the wrong
        // answer is the bug: the two candidates are not interchangeable for a blit.
        const GLenum reported = (componentType == GL_FLOAT || depthBits > 24) ? GL_DEPTH32F_STENCIL8
                                                                             : GL_DEPTH24_STENCIL8;

        GLuint fbo = 0;
        GLuint colorRbo = 0;
        GLuint depthRbo = 0;
        glGenFramebuffers(1, &fbo);
        glGenRenderbuffers(1, &colorRbo);
        glGenRenderbuffers(1, &depthRbo);
        glBindRenderbuffer(GL_RENDERBUFFER, colorRbo);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, width, height);
        glBindRenderbuffer(GL_RENDERBUFFER, depthRbo);
        glRenderbufferStorage(GL_RENDERBUFFER, reported, width, height);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, colorRbo);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depthRbo);
        EXPECT_EQ(FirstGLError(), 0u);
        ASSERT_TRUE(FramebufferIsComplete());

        // Put a known depth in the default framebuffer, then blit colour+depth+stencil out of
        // it into the buffer that its own description asked for.
        BindDefaultFramebuffer();
        glViewport(0, 0, width, height);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(0.0f, 1.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ClearDepthStencil();
        EXPECT_EQ(FirstGLError(), 0u);

        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
        glBlitFramebuffer(0, 0, width, height, 0, 0, width, height,
                          GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT, GL_NEAREST);
        EXPECT_EQ(FirstGLError(), 0u)
            << "blitting depth/stencil out of the default framebuffer into a buffer allocated from the format "
               "the default framebuffer itself reported was rejected - the report and the storage disagree";

        glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
        unsigned char color[4] = {0, 0, 0, 0};
        glReadPixels(width / 2, height / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, color);
        const float depth = ReadDepthAt(width / 2, height / 2);
        const int stencil = ReadStencilAt(width / 2, height / 2);
        EXPECT_EQ(FirstGLError(), 0u);
        // The colour bit is the precondition, not the claim: it says this stack can blit out of
        // its default framebuffer at all, which has nothing to do with depth/stencil formats.
        // DirectVulkan on a surfaceless pbuffer cannot - the whole call, colour included, is a
        // no-op there, while the same blit works on a real surface (KHR-GLxx.framebuffer_blit
        // exercises exactly it and Magma passes 33/33 on device). Skipping keeps the
        // depth/stencil claim below falsifiable instead of drowning it in an unrelated
        // harness limitation.
        if (int(color[1]) <= 192) {
            // GTEST_SKIP() expands to a return, so the teardown below it would never run and this
            // scenario would hand the next one a foreign framebuffer plus three leaked objects -
            // and this is the path DirectVulkan takes on every headless run, not a rare one.
            BindDefaultFramebuffer();
            glDeleteFramebuffers(1, &fbo);
            glDeleteRenderbuffers(1, &colorRbo);
            glDeleteRenderbuffers(1, &depthRbo);
            gl.EndFrame();
            GTEST_SKIP() << "backend " << gl.BackendName() << " on this surface transferred no colour either (green="
                         << int(color[1])
                         << "): it cannot blit out of the default framebuffer here, so the depth/stencil half proves "
                            "nothing. The GL-error assertion above still ran, and it is the format contract";
        }
        EXPECT_NEAR(depth, kDepthValue, 1.0f / 4096.0f)
            << "depth blitted out of the default framebuffer read back " << depth
            << (std::fabs(depth - kDepthPoison) < 1e-6f ? " - the blit transferred nothing" : "");
        EXPECT_EQ(stencil, kStencilValue) << "stencil blitted out of the default framebuffer read back " << stencil;

        BindDefaultFramebuffer();
        glDeleteFramebuffers(1, &fbo);
        glDeleteRenderbuffers(1, &colorRbo);
        glDeleteRenderbuffers(1, &depthRbo);
        gl.EndFrame();
    }

} // namespace MGITest
