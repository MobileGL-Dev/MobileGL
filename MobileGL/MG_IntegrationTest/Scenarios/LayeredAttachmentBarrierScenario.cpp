// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/LayeredAttachmentBarrierScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - A TRANSFER OFF A NON-ZERO ATTACHMENT LAYER READS THE LAYER THE BARRIER MOVED.
//
// Every transfer DirectVulkan performs against a framebuffer attachment is three commands: a
// barrier that puts the image in TRANSFER_SRC/DST, the copy or blit itself, and a barrier that
// puts it back. The copy names the attachment's layer - glFramebufferTextureLayer(.., layer) ends
// up in `srcSubresource.baseArrayLayer` - but TransitionImageLayout used to emit `layerCount = 1`
// from `baseArrayLayer 0`, so for every attachment on a layer above zero the barrier moved layer 0
// and the copy read layer N. The layer the transfer touched was never transitioned: it sat in
// COLOR_ATTACHMENT_OPTIMAL (or DEPTH_STENCIL_ATTACHMENT_OPTIMAL) while being read as TRANSFER_SRC.
//
// That is undefined behaviour, not a guaranteed wrong pixel: a layout is a compression/tiling
// promise, so a driver that stores both layouts identically returns the right bytes anyway. The
// software lanes (lavapipe) are exactly such a driver, which is why this scenario is paired with a
// validation-layer run - the layer names the mismatch outright
// (VUID-vkCmdCopyImageToBuffer-srcImageLayout-00189, "srcImageLayout ... doesn't match the actual
// current layout") where the pixels here cannot. On a tiler that really does re-tile per layout,
// these are the reads that come back as garbage.
//
// The four cases below are the four transfer paths that take an attachment layer from GL:
//
//   glReadPixels (colour)                    -> VulkanRenderer::ReadPixels
//   glBlitFramebuffer (colour)               -> VulkanRenderer::BlitNamedFramebuffer
//   glReadPixels (GL_DEPTH_COMPONENT)        -> VulkanRenderer::ReadDepthStencilImageToClient
//   glBlitFramebuffer (GL_DEPTH_BUFFER_BIT)  -> VulkanRenderer::BlitNamedFramebuffer, depth leg
//
// Each one renders or clears INTO the non-zero layer first, so the image is genuinely sitting in
// its attachment layout when the transfer starts - a scenario that only uploaded texels would
// leave it in a transfer layout already and the mismatched barrier would be a no-op.
//
// Every case also asserts the layers it did not name still hold their own fill, so a backend that
// "fixed" the miss by transferring the whole image passes neither half.
//
// DirectGLES is the control: it hands the same calls to the driver, so a failure on both backends
// means the scenario is wrong and a failure on DirectVulkan alone means Magma is.

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

        constexpr int kWidth = 8;
        constexpr int kHeight = 8;
        // Four layers with the subject at index 2: layers on both sides of it stay untouched, so
        // "moved the whole image" and "moved layer 0" are both distinguishable from correct.
        constexpr int kLayers = 4;
        constexpr int kSubjectLayer = 2;

        // A value no correct read can produce, so "the backend wrote nothing" fails loudly.
        constexpr float kDepthPoison = 0.2f;

        std::string Describe(const Rgba8& color) {
            return "(" + std::to_string(color.r) + ", " + std::to_string(color.g) + ", " + std::to_string(color.b) +
                   ", " + std::to_string(color.a) + ")";
        }

        // Per-layer fill, uniform within a layer: the defect is about WHICH layer is addressed, and
        // a value that also varied inside the layer would make the assertions depend on row order.
        Rgba8 LayerFill(int layer) {
            return {static_cast<GLubyte>(17 + layer * 30), static_cast<GLubyte>(200 - layer * 25),
                    static_cast<GLubyte>(60 + layer * 40), 255};
        }

        // What the draw paints - matches kFS below, and is deliberately none of the LayerFill
        // values so "the draw never landed" cannot read as a pass.
        constexpr Rgba8 kPaintedColor{26, 51, 204, 255};

        constexpr const char* kVS = R"(#version 330 core
in vec2 aPos;
void main() { gl_Position = vec4(aPos, 0.0, 1.0); }
)";

        constexpr const char* kFS = R"(#version 330 core
out vec4 o_color;
void main() { o_color = vec4(0.1, 0.2, 0.8, 1.0); }
)";

        void DrawFullViewportQuad(unsigned int program) {
            static const float kQuad[] = {-1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f};
            GLuint vao = 0, vbo = 0;
            glGenVertexArrays(1, &vao);
            glBindVertexArray(vao);
            glGenBuffers(1, &vbo);
            glBindBuffer(GL_ARRAY_BUFFER, vbo);
            glBufferData(GL_ARRAY_BUFFER, sizeof(kQuad), kQuad, GL_STATIC_DRAW);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
            glUseProgram(program);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            glBindVertexArray(0);
            glDeleteBuffers(1, &vbo);
            glDeleteVertexArrays(1, &vao);
        }

        class LayeredAttachmentBarrierScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                std::string error;
                m_program = CompileProgram(kVS, kFS, &error);
                ASSERT_NE(m_program, 0u) << error;
            }

            void TearDown() override {
                if (!Ready()) return;
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                for (const GLuint fbo : m_fbos) {
                    glDeleteFramebuffers(1, &fbo);
                }
                m_fbos.clear();
                for (const GLuint texture : m_textures) {
                    glDeleteTextures(1, &texture);
                }
                m_textures.clear();
                if (m_program != 0) {
                    glUseProgram(0);
                    glDeleteProgram(m_program);
                    m_program = 0;
                }
            }

            // An RGBA8 2D array with a different uniform colour per layer.
            GLuint MakeColorArray() {
                GLuint texture = 0;
                glGenTextures(1, &texture);
                m_textures.push_back(texture);
                glBindTexture(GL_TEXTURE_2D_ARRAY, texture);
                glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_RGBA8, kWidth, kHeight, kLayers);
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                for (int layer = 0; layer < kLayers; ++layer) {
                    const std::vector<Rgba8> texels(static_cast<std::size_t>(kWidth) * kHeight, LayerFill(layer));
                    glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, layer, kWidth, kHeight, 1, GL_RGBA,
                                    GL_UNSIGNED_BYTE, texels.data());
                }
                glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
                return texture;
            }

            // A depth 2D array. No initial upload: depth arrays are filled by clearing through an
            // attachment, which is also the state the transfer paths have to cope with.
            GLuint MakeDepthArray() {
                GLuint texture = 0;
                glGenTextures(1, &texture);
                m_textures.push_back(texture);
                glBindTexture(GL_TEXTURE_2D_ARRAY, texture);
                glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_DEPTH_COMPONENT24, kWidth, kHeight, kLayers);
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
                return texture;
            }

            // One FBO naming `layer` of the given arrays. Depth is optional (0 = colour only).
            GLuint MakeLayerFbo(GLuint colorArray, GLuint depthArray, int layer) {
                GLuint fbo = 0;
                glGenFramebuffers(1, &fbo);
                m_fbos.push_back(fbo);
                glBindFramebuffer(GL_FRAMEBUFFER, fbo);
                glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, colorArray, 0, layer);
                if (depthArray != 0) {
                    glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, depthArray, 0, layer);
                }
                EXPECT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE))
                    << "layer " << layer << " is not attachable";
                return fbo;
            }

            // glReadPixels of one whole layer, through an FBO that names it.
            Rgba8 ReadLayer(GLuint colorArray, int layer) {
                const GLuint fbo = MakeLayerFbo(colorArray, 0, layer);
                glBindFramebuffer(GL_FRAMEBUFFER, fbo);
                glReadBuffer(GL_COLOR_ATTACHMENT0);
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                std::vector<Rgba8> pixels(static_cast<std::size_t>(kWidth) * kHeight, Rgba8{});
                glReadPixels(0, 0, kWidth, kHeight, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                // The fill is uniform within a layer, so any disagreement between texels is itself
                // a failure - reported here rather than silently reduced to pixels[0].
                for (std::size_t i = 1; i < pixels.size(); ++i) {
                    EXPECT_TRUE(pixels[i] == pixels[0])
                        << "layer " << layer << " is not uniform: texel 0 is " << Describe(pixels[0]) << ", texel "
                        << i << " is " << Describe(pixels[i]);
                }
                return pixels[0];
            }

            // Every layer but `changed` still holds its own fill.
            void ExpectOtherLayersUntouched(GLuint colorArray, int changed, const char* what) {
                for (int layer = 0; layer < kLayers; ++layer) {
                    if (layer == changed) continue;
                    const Rgba8 actual = ReadLayer(colorArray, layer);
                    EXPECT_TRUE(actual == LayerFill(layer))
                        << what << ": layer " << layer << " should still hold its fill but is " << Describe(actual)
                        << ", expected " << Describe(LayerFill(layer));
                }
            }

            float ReadDepthAt(int x, int y) const {
                float depth = kDepthPoison;
                glReadPixels(x, y, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &depth);
                return depth;
            }

            std::vector<GLuint> m_textures;
            std::vector<GLuint> m_fbos;
            unsigned int m_program = 0;
        };

        // glReadPixels straight off a layer that was just rendered to. The image is in
        // COLOR_ATTACHMENT_OPTIMAL when the readback barrier runs, so the barrier and the copy
        // disagreeing about the layer is a live layout mismatch, not a bookkeeping detail.
        TEST_F(LayeredAttachmentBarrierScenario, ReadPixelsOffRenderedNonZeroLayer) {
            if (!Ready()) return;

            const GLuint colorArray = MakeColorArray();
            ASSERT_EQ(FirstGLError(), 0u) << "texture setup failed";

            const GLuint fbo = MakeLayerFbo(colorArray, 0, kSubjectLayer);
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glViewport(0, 0, kWidth, kHeight);
            glDisable(GL_SCISSOR_TEST);
            glDisable(GL_DEPTH_TEST);
            glDrawBuffer(GL_COLOR_ATTACHMENT0);
            DrawFullViewportQuad(m_program);

            glReadBuffer(GL_COLOR_ATTACHMENT0);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            std::vector<Rgba8> pixels(static_cast<std::size_t>(kWidth) * kHeight, Rgba8{});
            glReadPixels(0, 0, kWidth, kHeight, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            EXPECT_EQ(FirstGLError(), 0u);

            for (std::size_t i = 0; i < pixels.size(); ++i) {
                ASSERT_NEAR(pixels[i].r, kPaintedColor.r, 2)
                    << "texel " << i << " of the rendered layer is " << Describe(pixels[i]);
                ASSERT_NEAR(pixels[i].g, kPaintedColor.g, 2) << "texel " << i;
                ASSERT_NEAR(pixels[i].b, kPaintedColor.b, 2) << "texel " << i;
            }

            ExpectOtherLayersUntouched(colorArray, kSubjectLayer, "readback off a rendered layer");
        }

        // glBlitFramebuffer between two non-zero layers of two different arrays. Both endpoints are
        // above layer 0, so the source and destination barriers are each wrong on their own side.
        TEST_F(LayeredAttachmentBarrierScenario, BlitBetweenNonZeroColorLayers) {
            if (!Ready()) return;

            const GLuint sourceArray = MakeColorArray();
            const GLuint destinationArray = MakeColorArray();
            ASSERT_EQ(FirstGLError(), 0u) << "texture setup failed";

            constexpr int kSourceLayer = 3;
            constexpr int kDestinationLayer = 1;

            const GLuint sourceFbo = MakeLayerFbo(sourceArray, 0, kSourceLayer);
            glBindFramebuffer(GL_FRAMEBUFFER, sourceFbo);
            glViewport(0, 0, kWidth, kHeight);
            glDisable(GL_SCISSOR_TEST);
            glDisable(GL_DEPTH_TEST);
            glDrawBuffer(GL_COLOR_ATTACHMENT0);
            DrawFullViewportQuad(m_program);

            const GLuint destinationFbo = MakeLayerFbo(destinationArray, 0, kDestinationLayer);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, sourceFbo);
            glReadBuffer(GL_COLOR_ATTACHMENT0);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, destinationFbo);
            glDrawBuffer(GL_COLOR_ATTACHMENT0);
            glBlitFramebuffer(0, 0, kWidth, kHeight, 0, 0, kWidth, kHeight, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            EXPECT_EQ(FirstGLError(), 0u);

            const Rgba8 blitted = ReadLayer(destinationArray, kDestinationLayer);
            EXPECT_NEAR(blitted.r, kPaintedColor.r, 2) << "blit destination layer is " << Describe(blitted);
            EXPECT_NEAR(blitted.g, kPaintedColor.g, 2);
            EXPECT_NEAR(blitted.b, kPaintedColor.b, 2);

            ExpectOtherLayersUntouched(destinationArray, kDestinationLayer, "colour blit destination");
            // The source layer was rendered, not blitted into, so it is checked separately.
            const Rgba8 source = ReadLayer(sourceArray, kSourceLayer);
            EXPECT_NEAR(source.r, kPaintedColor.r, 2) << "blit source layer is " << Describe(source);
            ExpectOtherLayersUntouched(sourceArray, kSourceLayer, "colour blit source");
        }

        // The depth aspect of the same readback path: the depth image sits in
        // DEPTH_STENCIL_ATTACHMENT_OPTIMAL after the clear, and the copy names the attached layer.
        TEST_F(LayeredAttachmentBarrierScenario, ReadDepthOffClearedNonZeroLayer) {
            if (!Ready()) return;

            const GLuint colorArray = MakeColorArray();
            const GLuint depthArray = MakeDepthArray();
            ASSERT_EQ(FirstGLError(), 0u) << "texture setup failed";

            const GLuint fbo = MakeLayerFbo(colorArray, depthArray, kSubjectLayer);
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glViewport(0, 0, kWidth, kHeight);
            glDisable(GL_SCISSOR_TEST);
            glDepthMask(GL_TRUE);
            glClearDepth(0.375);
            glClear(GL_DEPTH_BUFFER_BIT);

            const float centre = ReadDepthAt(kWidth / 2, kHeight / 2);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            EXPECT_EQ(FirstGLError(), 0u);
            EXPECT_NEAR(centre, 0.375f, 1.0f / 4096.0f)
                << "glReadPixels(GL_DEPTH_COMPONENT) off layer " << kSubjectLayer << " returned " << centre
                << (std::fabs(centre - kDepthPoison) < 1e-6f ? " - the destination was never written at all" : "");
        }

        // The depth leg of the blit path, both endpoints above layer 0. Verified by reading the
        // destination's depth back, which is the same readback the case above pins - so a failure
        // here with that one passing is the blit, not the readback.
        TEST_F(LayeredAttachmentBarrierScenario, BlitDepthBetweenNonZeroLayers) {
            if (!Ready()) return;

            const GLuint sourceColor = MakeColorArray();
            const GLuint sourceDepth = MakeDepthArray();
            const GLuint destinationColor = MakeColorArray();
            const GLuint destinationDepth = MakeDepthArray();
            ASSERT_EQ(FirstGLError(), 0u) << "texture setup failed";

            constexpr int kSourceLayer = 3;
            constexpr int kDestinationLayer = 1;

            const GLuint sourceFbo = MakeLayerFbo(sourceColor, sourceDepth, kSourceLayer);
            glBindFramebuffer(GL_FRAMEBUFFER, sourceFbo);
            glViewport(0, 0, kWidth, kHeight);
            glDisable(GL_SCISSOR_TEST);
            glDepthMask(GL_TRUE);
            glClearDepth(0.625);
            glClear(GL_DEPTH_BUFFER_BIT);

            // A destination pre-cleared to something the blit must overwrite, so "the blit did
            // nothing" and "the blit landed" are different answers.
            const GLuint destinationFbo = MakeLayerFbo(destinationColor, destinationDepth, kDestinationLayer);
            glBindFramebuffer(GL_FRAMEBUFFER, destinationFbo);
            glViewport(0, 0, kWidth, kHeight);
            glDepthMask(GL_TRUE);
            glClearDepth(0.125);
            glClear(GL_DEPTH_BUFFER_BIT);

            glBindFramebuffer(GL_READ_FRAMEBUFFER, sourceFbo);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, destinationFbo);
            glBlitFramebuffer(0, 0, kWidth, kHeight, 0, 0, kWidth, kHeight, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
            EXPECT_EQ(FirstGLError(), 0u);

            glBindFramebuffer(GL_FRAMEBUFFER, destinationFbo);
            const float blitted = ReadDepthAt(kWidth / 2, kHeight / 2);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            EXPECT_EQ(FirstGLError(), 0u);
            EXPECT_NEAR(blitted, 0.625f, 1.0f / 4096.0f)
                << "depth blitted onto layer " << kDestinationLayer << " reads back as " << blitted
                << (std::fabs(blitted - 0.125f) < 1e-3f ? " - the destination kept its own clear" : "");
        }

    } // namespace
} // namespace MGITest
