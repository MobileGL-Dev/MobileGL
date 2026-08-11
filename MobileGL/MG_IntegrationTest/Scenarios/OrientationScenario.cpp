// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/OrientationScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario A - "the frame came out upside down".
//
// The shipped bug (DirectVulkan, GetBaseTransformFlagsRaw): the shader
// transform flags - the Y-flip and surface-rotation bits that apply ONLY when
// the bound draw framebuffer is the default one - were memoized on the
// swapchain pre-transform alone. The is-default-framebuffer input was not part
// of the key, so whichever kind of pass evaluated the memo first decided the
// orientation of every pass after it. In a real frame that meant: after any
// render-to-texture pass, the next default-framebuffer pass inherited the FBO's
// unflipped flags and the whole frame rendered upside down (retrace SSIM 0.052,
// deterministic; flickering clouds on device).
//
// What pins it: a pattern asymmetric in BOTH axes - four quadrants, coloured
//
//        top-left  RED  | WHITE  top-right
//     bottom-left BLUE  | GREEN  bottom-right
//
// - drawn to a target, read back with glReadPixels, and reduced to the four
// quadrant-centre colours in the fixed order bottom-left, bottom-right,
// top-left, top-right.
//
// Four quadrants rather than the three horizontal stripes this scenario used to
// draw, because stripes only pin ONE axis. Stripes read down the centre line
// are unchanged by an X flip, by a transpose, and by a 180 rotation composed
// with a Y flip: all three of those bugs would have rendered a green stripe
// between a blue one and a red one and passed. Every one of the eight
// symmetries of the square now produces a different string:
//
//     identity       blue,green,red,white     <- correct
//     Y flip         red,white,blue,green     <- the shipped bug
//     X flip         green,blue,white,red
//     180 rotation   white,red,green,blue
//     transpose      blue,red,green,white
//     anti-transpose white,green,red,blue
//     rotate 90 CCW  red,blue,white,green
//     rotate 90 CW   green,white,blue,red
//
// The assertions then go further than the signature: every quadrant is checked
// pixel by pixel over its whole area (RegionIsMostly), so a partial or torn
// draw cannot pass by having the four sampled centres come out right.
//
// Both orderings are covered, because the memo is poisoned by whichever pass
// runs first and these tests share one process:
//   - default -> FBO -> default   (the FBO pass inherits the default's flip)
//   - FBO -> default              (the shipped symptom: the default pass
//                                  inherits the FBO's lack of flip)

#include <algorithm>
#include <cstdint>
#include <cstring>
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

        constexpr const char* kVertexSource = R"(#version 330 core
in vec2 aPos;
in vec3 aColor;
out vec3 vColor;
void main() {
    vColor = aColor;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

        constexpr const char* kFragmentSource = R"(#version 330 core
in vec3 vColor;
out vec4 oColor;
void main() {
    oColor = vec4(vColor, 1.0);
}
)";

        // The correctly-oriented answer, in glReadPixels order (row 0 is the
        // bottom row) and in QuadrantSignature's order: bottom-left, bottom-right,
        // top-left, top-right. Plain GL semantics; holds for every framebuffer,
        // default or not.
        constexpr const char* kUprightSignature = "blue,green,red,white";

        // How far inside each quadrant the whole-region checks start. The quadrant
        // seam sits on a pixel boundary, so one pixel of margin is enough to make
        // "every single pixel" an achievable (and therefore useful) demand.
        constexpr int kQuadrantInset = 2;

        // A deliberately asymmetric sub-rect of the 128x96 surface: neither centred nor
        // full-extent in either axis, mirroring the conformance suite's randomised
        // sub-viewport geometry (glcShaderRenderCase.cpp:735-741). Asymmetry is the whole
        // point - y == H - y - h is exactly the case an unconverted Y origin gets right by
        // accident, and it is the only case the shipped code ever exercised.
        //   correct band  = GL rows [13, 55)
        //   mirrored band = GL rows [41, 83)   (what H-y-h produces)
        constexpr int kSubX = 17;
        constexpr int kSubY = 13;
        constexpr int kSubW = 60;
        constexpr int kSubH = 42;

        Image CropRect(const Image& source, int x0, int y0, int width, int height) {
            Image out(width, height);
            const std::size_t rowBytes = static_cast<std::size_t>(width) * 4;
            for (int y = 0; y < height; ++y) {
                const std::uint8_t* sourceRow =
                    source.Data() + (static_cast<std::size_t>(y0 + y) * source.Width() + x0) * 4;
                std::memcpy(out.Data() + static_cast<std::size_t>(y) * rowBytes, sourceRow, rowBytes);
            }
            return out;
        }

        Image VFlip(const Image& source) {
            Image out(source.Width(), source.Height());
            const std::size_t rowBytes = static_cast<std::size_t>(source.Width()) * 4;
            for (int y = 0; y < source.Height(); ++y) {
                std::memcpy(out.Data() + static_cast<std::size_t>(y) * rowBytes,
                            source.Data() + static_cast<std::size_t>(source.Height() - 1 - y) * rowBytes, rowBytes);
            }
            return out;
        }

        struct Vertex {
            float x, y;
            float r, g, b;
        };

        void AppendQuad(std::vector<Vertex>& out, float x0, float x1, float y0, float y1, float r, float g, float b) {
            const Vertex bl{x0, y0, r, g, b};
            const Vertex br{x1, y0, r, g, b};
            const Vertex tr{x1, y1, r, g, b};
            const Vertex tl{x0, y1, r, g, b};
            out.insert(out.end(), {bl, br, tr, bl, tr, tl});
        }

        std::vector<Vertex> QuadrantGeometry() {
            std::vector<Vertex> vertices;
            vertices.reserve(24);
            AppendQuad(vertices, -1.0f, 0.0f, -1.0f, 0.0f, 0.0f, 0.0f, 1.0f); // bottom-left: blue
            AppendQuad(vertices, 0.0f, 1.0f, -1.0f, 0.0f, 0.0f, 1.0f, 0.0f);  // bottom-right: green
            AppendQuad(vertices, -1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f);  // top-left: red
            AppendQuad(vertices, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f);   // top-right: white
            return vertices;
        }

        class OrientationScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;

                std::string error;
                m_program = CompileProgram(kVertexSource, kFragmentSource, &error);
                ASSERT_NE(m_program, 0u) << error;

                const std::vector<Vertex> vertices = QuadrantGeometry();
                m_vertexCount = static_cast<int>(vertices.size());
                glGenVertexArrays(1, &m_vao);
                glBindVertexArray(m_vao);
                glGenBuffers(1, &m_vbo);
                glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
                glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(vertices.size() * sizeof(Vertex)), vertices.data(),
                             GL_STATIC_DRAW);
                glEnableVertexAttribArray(0);
                glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(0));
                glEnableVertexAttribArray(1);
                glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(8));
                glBindVertexArray(0);

                m_offscreen = MakeColorFbo(Gl().Width(), Gl().Height());
                ASSERT_NE(m_offscreen.fbo, 0u) << "offscreen FBO is not framebuffer-complete";

                ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "setup left a GL error behind";
            }

            void TearDown() override {
                if (!Ready()) return;
                DestroyColorFbo(m_offscreen);
                if (m_vbo != 0) glDeleteBuffers(1, &m_vbo);
                if (m_vao != 0) glDeleteVertexArrays(1, &m_vao);
                if (m_program != 0) glDeleteProgram(m_program);
            }

            void DrawQuadrants() {
                glDisable(GL_DEPTH_TEST);
                glDisable(GL_BLEND);
                glUseProgram(m_program);
                glBindVertexArray(m_vao);
                glDrawArrays(GL_TRIANGLES, 0, m_vertexCount);
                glBindVertexArray(0);
            }

            // One pass to the default (presentable) framebuffer.
            Image DefaultFramebufferPass() {
                BindDefaultFramebuffer();
                ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
                DrawQuadrants();
                return ReadPixels(Gl().Width(), Gl().Height());
            }

            // One render-to-texture pass. Real frames do this constantly
            // (shadow maps, post-processing, Minecraft's main render target).
            Image OffscreenPass() {
                BindFbo(m_offscreen);
                ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
                DrawQuadrants();
                return ReadPixels(m_offscreen.width, m_offscreen.height);
            }

            // The signature says WHICH transform went wrong; this says the whole
            // image is right, not merely its four sampled centres.
            void ExpectUprightQuadrants(const Image& image, const std::string& when) {
                const int w = image.Width();
                const int h = image.Height();
                const int inset = kQuadrantInset;
                EXPECT_TRUE(RegionIsMostly(image, inset, w / 2 - inset, inset, h / 2 - inset, "blue", 0.0, when));
                EXPECT_TRUE(RegionIsMostly(image, w / 2 + inset, w - inset, inset, h / 2 - inset, "green", 0.0, when));
                EXPECT_TRUE(RegionIsMostly(image, inset, w / 2 - inset, h / 2 + inset, h - inset, "red", 0.0, when));
                EXPECT_TRUE(RegionIsMostly(image, w / 2 + inset, w - inset, h / 2 + inset, h - inset, "white", 0.0,
                                           when));
            }

            unsigned int m_program = 0;
            unsigned int m_vao = 0;
            unsigned int m_vbo = 0;
            int m_vertexCount = 0;
            ColorFbo m_offscreen;
        };

        // The plain statement of GL semantics that everything else leans on: an
        // FBO pass is never flipped.
        TEST_F(OrientationScenario, OffscreenPassRendersUpright) {
            const Image offscreen = OffscreenPass();
            EXPECT_EQ(offscreen.QuadrantSignature(), kUprightSignature)
                << "a render-to-texture pass must render unflipped";
            ExpectUprightQuadrants(offscreen, "render-to-texture pass");
            EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
        }

        // The same for the default framebuffer: whatever the backend does with
        // the swapchain internally, glReadPixels owes the caller GL orientation.
        TEST_F(OrientationScenario, DefaultFramebufferPassRendersUpright) {
            const Image presented = DefaultFramebufferPass();
            EXPECT_EQ(presented.QuadrantSignature(), kUprightSignature)
                << "a default-framebuffer pass must read back in GL orientation";
            ExpectUprightQuadrants(presented, "default-framebuffer pass");
            EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
        }

        // Scenario A proper: default -> FBO -> default in one frame. The third
        // pass must be pixel-identical to the first; the FBO pass in between
        // must not have moved anything.
        TEST_F(OrientationScenario, DefaultFramebufferSurvivesAnOffscreenPass) {
            const Image before = DefaultFramebufferPass();
            const Image offscreen = OffscreenPass();
            const Image after = DefaultFramebufferPass();

            EXPECT_EQ(before.QuadrantSignature(), kUprightSignature)
                << "first default-framebuffer pass is already misoriented";
            EXPECT_EQ(offscreen.QuadrantSignature(), kUprightSignature)
                << "the render-to-texture pass in the middle rendered flipped - the "
                   "default framebuffer's transform flags leaked into it";
            EXPECT_EQ(after.QuadrantSignature(), kUprightSignature)
                << "the default-framebuffer pass AFTER a render-to-texture pass is "
                   "misoriented - it inherited the FBO's transform flags";
            ExpectUprightQuadrants(after, "default-framebuffer pass after a render-to-texture pass");
            EXPECT_TRUE(after == before) << "the third pass differs from the first in " << after.ByteDiffCount(before)
                                         << " bytes; first=" << before.QuadrantSignature()
                                         << " third=" << after.QuadrantSignature();
            EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
        }

        // The shipped symptom, in its shipped order: an FBO pass, then the
        // default framebuffer. This is the one that flipped whole Minecraft
        // frames.
        TEST_F(OrientationScenario, DefaultFramebufferAfterOffscreenIsNotFlipped) {
            const Image offscreen = OffscreenPass();
            const Image presented = DefaultFramebufferPass();

            EXPECT_EQ(offscreen.QuadrantSignature(), kUprightSignature)
                << "render-to-texture pass rendered flipped";
            EXPECT_EQ(presented.QuadrantSignature(), kUprightSignature)
                << "the default-framebuffer pass that follows a render-to-texture pass "
                   "rendered upside down";
            ExpectUprightQuadrants(presented, "default-framebuffer pass following a render-to-texture pass");
            EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
        }

        // And across a real frame boundary, which is how a game actually
        // alternates the two kinds of pass.
        TEST_F(OrientationScenario, OrientationIsStableAcrossFrames) {
            const Image firstFrame = DefaultFramebufferPass();
            ExpectUprightQuadrants(firstFrame, "frame 0");
            Gl().EndFrame();

            for (int frame = 0; frame < 3; ++frame) {
                const Image offscreen = OffscreenPass();
                EXPECT_EQ(offscreen.QuadrantSignature(), kUprightSignature)
                    << "frame " << frame + 1 << "'s render-to-texture pass is misoriented";
                const Image presented = DefaultFramebufferPass();
                EXPECT_EQ(presented.QuadrantSignature(), kUprightSignature)
                    << "frame " << frame + 1 << " of the alternating FBO/default loop is misoriented";
                ExpectUprightQuadrants(presented, "frame " + std::to_string(frame + 1));
                EXPECT_TRUE(presented == firstFrame) << "frame " << frame + 1 << " differs from frame 0 in "
                                                     << presented.ByteDiffCount(firstFrame) << " bytes";
                Gl().EndFrame();
            }
            EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
        }

        // A standing self-test of the signature, not of MobileGL: it proves the
        // four-quadrant reduction really does separate all eight symmetries of
        // the square, so a future "simplify the pattern" change cannot quietly
        // reintroduce the blind spot the three-stripe version had (X flip,
        // transpose and 180+Y-flip all left the stripe signature alone).
        TEST_F(OrientationScenario, QuadrantSignatureSeparatesEverySquareSymmetry) {
            const Image upright = OffscreenPass();
            ASSERT_EQ(upright.QuadrantSignature(), kUprightSignature) << "the reference image is not upright";

            const int w = upright.Width();
            const int h = upright.Height();
            // Transposes are expressed on the largest centred square the readback
            // contains, which is enough for the four quadrant centres to move.
            const int side = std::min(w, h);
            const int ox = (w - side) / 2;
            const int oy = (h - side) / 2;

            struct Symmetry {
                const char* name;
                const char* expected;
                int (*mapX)(int x, int y, int w, int h);
                int (*mapY)(int x, int y, int w, int h);
            };
            const Symmetry symmetries[] = {
                {"Y flip", "red,white,blue,green", [](int x, int, int, int) { return x; },
                 [](int, int y, int, int hh) { return hh - 1 - y; }},
                {"X flip", "green,blue,white,red", [](int x, int, int ww, int) { return ww - 1 - x; },
                 [](int, int y, int, int) { return y; }},
                {"180 rotation", "white,red,green,blue", [](int x, int, int ww, int) { return ww - 1 - x; },
                 [](int, int y, int, int hh) { return hh - 1 - y; }},
            };

            for (const Symmetry& symmetry : symmetries) {
                Image transformed(w, h);
                for (int y = 0; y < h; ++y) {
                    for (int x = 0; x < w; ++x) {
                        const Rgba8 source = upright.At(symmetry.mapX(x, y, w, h), symmetry.mapY(x, y, w, h));
                        std::uint8_t* out = transformed.Data() + (std::size_t(y) * w + x) * 4;
                        out[0] = source.r;
                        out[1] = source.g;
                        out[2] = source.b;
                        out[3] = source.a;
                    }
                }
                EXPECT_EQ(transformed.QuadrantSignature(), symmetry.expected)
                    << symmetry.name << " must produce its own signature, or the pattern cannot see it";
                EXPECT_NE(transformed.QuadrantSignature(), kUprightSignature)
                    << symmetry.name << " is INDISTINGUISHABLE from an upright frame - the pattern is too symmetric";
            }

            // The four symmetries that move the axes into each other. They only
            // make sense on a square, so they run on the largest centred one.
            struct SquareSymmetry {
                const char* name;
                const char* expected;
                int (*sourceX)(int x, int y, int side);
                int (*sourceY)(int x, int y, int side);
            };
            const SquareSymmetry squareSymmetries[] = {
                {"transpose", "blue,red,green,white", [](int, int y, int) { return y; },
                 [](int x, int, int) { return x; }},
                {"anti-transpose", "white,green,red,blue", [](int, int y, int s) { return s - 1 - y; },
                 [](int x, int, int s) { return s - 1 - x; }},
                {"rotate 90 CCW", "red,blue,white,green", [](int, int y, int) { return y; },
                 [](int x, int, int s) { return s - 1 - x; }},
                {"rotate 90 CW", "green,white,blue,red", [](int, int y, int s) { return s - 1 - y; },
                 [](int x, int, int) { return x; }},
            };
            for (const SquareSymmetry& symmetry : squareSymmetries) {
                Image square(side, side);
                for (int y = 0; y < side; ++y) {
                    for (int x = 0; x < side; ++x) {
                        const Rgba8 source =
                            upright.At(ox + symmetry.sourceX(x, y, side), oy + symmetry.sourceY(x, y, side));
                        std::uint8_t* out = square.Data() + (std::size_t(y) * side + x) * 4;
                        out[0] = source.r;
                        out[1] = source.g;
                        out[2] = source.b;
                        out[3] = source.a;
                    }
                }
                EXPECT_EQ(square.QuadrantSignature(), symmetry.expected)
                    << symmetry.name << " must produce its own signature, or the pattern cannot see it";
                EXPECT_NE(square.QuadrantSignature(), kUprightSignature)
                    << symmetry.name << " is INDISTINGUISHABLE from an upright frame";
            }
        }

        // ------------------------------------------------------------------ sub-rect / M-1 ----
        //
        // Everything above reads the FULL extent of its target, which is the one case
        // DirectVulkan's default-framebuffer readback ever re-oriented: the remap at
        // VulkanRenderer.cpp:2042 had no rect parameters at all, so :8278 gated it on
        // `width == swapchainExtent.width && height == swapchainExtent.height` and fell back to a
        // raw copy otherwise. Meanwhile the viewport (:422), the scissor (:506-546) and the
        // ReadPixels copy offset (:8238) all used the GL bottom-origin Y verbatim as a Vulkan
        // top-origin Y.
        //
        // In the conformance suite those defects CANCEL in placement - the draw lands in Vulkan
        // rows [y, y+h) and the readback copies the same rows back - and compose into an exact
        // vertical flip of a correct image. That is 1,759 of Magma's 1,793 non-pass cases, and
        // image forensics over all 861 gl33 failures found 861 vertical flips and nothing else.
        // Taken apart, they are two independent user-visible bugs, so they are tested apart:
        // SubViewportDraw pins placement with a full-extent read, SubRectReadback pins the
        // readback rect after a full-viewport draw, and SubViewportSubRectRoundTrip is the CTS
        // shape where the two cancel.

        // Placement: a sub-viewport draw must land in GL rows [y0, y0+h), not mirrored about the
        // surface centre. Read back full-extent, which is the path that already worked, so a
        // failure here can only be the viewport's Y origin.
        TEST_F(OrientationScenario, SubViewportDrawLandsWhereGLPutsIt) {
            BindDefaultFramebuffer();
            ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
            glViewport(kSubX, kSubY, kSubW, kSubH);
            DrawQuadrants();
            glViewport(0, 0, Gl().Width(), Gl().Height());

            const Image whole = ReadPixels(Gl().Width(), Gl().Height());
            EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));

            const Image placed = CropRect(whole, kSubX, kSubY, kSubW, kSubH);
            EXPECT_EQ(placed.QuadrantSignature(), kUprightSignature)
                << "the sub-viewport draw is not upright inside its own rect";
            ExpectUprightQuadrants(placed, "sub-viewport draw, cropped out of a full-extent read");

            // Nothing may have been painted outside the viewport. This is what catches the
            // mirrored placement: the drawn band would sit at GL rows [41, 83) instead.
            EXPECT_TRUE(RegionIsMostly(whole, 0, Gl().Width() - 1, 0, kSubY - 2, "black", 0.0,
                                       "below the sub-viewport"));
            EXPECT_TRUE(RegionIsMostly(whole, 0, Gl().Width() - 1, kSubY + kSubH + 1, Gl().Height() - 1, "black",
                                       0.0, "above the sub-viewport"));
        }

        // Readback: a full-viewport draw read back through a sub-rect must return the requested
        // band, in GL row order. Band and orientation are asserted separately so that fixing only
        // one of the two cannot pass this case.
        TEST_F(OrientationScenario, SubRectReadbackReturnsTheRequestedBandUpright) {
            BindDefaultFramebuffer();
            ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
            DrawQuadrants();

            const Image whole = ReadPixels(Gl().Width(), Gl().Height());
            ASSERT_EQ(whole.QuadrantSignature(), kUprightSignature)
                << "the full-extent read is already wrong, so nothing below can be trusted";

            const Image sub = ReadPixelsRect(kSubX, kSubY, kSubW, kSubH);
            EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
            ASSERT_EQ(sub.Width(), kSubW);
            ASSERT_EQ(sub.Height(), kSubH);

            const Image requestedBand = CropRect(whole, kSubX, kSubY, kSubW, kSubH);
            const Image mirroredBand = CropRect(whole, kSubX, Gl().Height() - kSubY - kSubH, kSubW, kSubH);

            // The geometry has to be able to see both mistakes; if a future surface size made the
            // band symmetric these assertions would be vacuous, so say so loudly instead.
            ASSERT_FALSE(requestedBand == VFlip(requestedBand))
                << "the chosen sub-rect is vertically symmetric - it cannot detect a row flip";
            ASSERT_FALSE(requestedBand == mirroredBand)
                << "the chosen sub-rect equals its mirror band - it cannot detect a wrong band";

            EXPECT_FALSE(sub == VFlip(requestedBand))
                << "ORIENTATION: the requested band came back with its rows in Vulkan (top-first) order";
            EXPECT_FALSE(sub == mirroredBand || sub == VFlip(mirroredBand))
                << "BAND: the read returned GL rows [H-y-h, H-y) instead of [y, y+h)";
            EXPECT_TRUE(sub == requestedBand)
                << "the sub-rect readback differs from the same rect of the full-extent read in "
                << sub.ByteDiffCount(requestedBand) << " bytes";
        }

        // The exact conformance-suite shape: an asymmetric sub-viewport draw read back through the
        // very same sub-rect. The placement and readback errors cancel, leaving an image that is
        // correct in every pixel VALUE and vertically flipped - which is precisely the 861-case
        // signature. One assertion, and it pins all of them.
        TEST_F(OrientationScenario, SubViewportSubRectRoundTripIsUpright) {
            BindDefaultFramebuffer();
            ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
            glViewport(kSubX, kSubY, kSubW, kSubH);
            DrawQuadrants();
            const Image sub = ReadPixelsRect(kSubX, kSubY, kSubW, kSubH);
            glViewport(0, 0, Gl().Width(), Gl().Height());

            EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
            EXPECT_EQ(sub.QuadrantSignature(), kUprightSignature)
                << "sub-viewport draw + same-rect readback came back flipped - this is the shape "
                   "behind KHR-GL33/GL40.shaders.* (861 cases each)";
            ExpectUprightQuadrants(sub, "sub-viewport draw read back through the same sub-rect");
        }

        // The same conversion, on the other rect consumer that reads the default framebuffer.
        // glBlitFramebuffer already converted its DESTINATION rect when the draw framebuffer was
        // the default one (ApplyNativeBlitDefaultFramebufferTransform), but never its SOURCE rect,
        // so a blit OUT of the default framebuffer took the mirrored band and wrote it upside
        // down. Blitting a sub-rect and comparing against the same sub-rect of a direct read pins
        // both halves at once.
        TEST_F(OrientationScenario, BlitOutOfTheDefaultFramebufferKeepsBandAndOrientation) {
            BindDefaultFramebuffer();
            ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
            DrawQuadrants();
            const Image whole = ReadPixels(Gl().Width(), Gl().Height());
            ASSERT_EQ(whole.QuadrantSignature(), kUprightSignature)
                << "the full-extent read is already wrong, so nothing below can be trusted";

            BindFbo(m_offscreen);
            ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_offscreen.fbo);
            glBlitFramebuffer(kSubX, kSubY, kSubX + kSubW, kSubY + kSubH, kSubX, kSubY, kSubX + kSubW,
                              kSubY + kSubH, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            const unsigned int blitError = FirstGLError();
            if (blitError != GL_NO_ERROR) {
                GTEST_SKIP() << "this backend refused the default-framebuffer blit: "
                             << GLErrorName(blitError);
            }

            glBindFramebuffer(GL_FRAMEBUFFER, m_offscreen.fbo);
            const Image blitted = ReadPixels(m_offscreen.width, m_offscreen.height);
            EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));

            const Image landed = CropRect(blitted, kSubX, kSubY, kSubW, kSubH);
            const Image expected = CropRect(whole, kSubX, kSubY, kSubW, kSubH);
            EXPECT_FALSE(landed == VFlip(expected))
                << "ORIENTATION: the blitted band arrived upside down";
            EXPECT_TRUE(landed == expected)
                << "the blitted sub-rect differs from the same sub-rect of a direct read in "
                << landed.ByteDiffCount(expected) << " bytes";
        }

        // Negative control. A non-default framebuffer is already self-consistent - no
        // gl_Position.y negation, GL row 0 IS Vulkan row 0 - so none of the fixes above may touch
        // it. If this ever starts failing, the default-FBO remap has leaked into the FBO path.
        TEST_F(OrientationScenario, FboSubRectReadbackAndSubViewportAreUnaffected) {
            BindFbo(m_offscreen);
            ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
            DrawQuadrants();

            const Image whole = ReadPixels(m_offscreen.width, m_offscreen.height);
            const Image sub = ReadPixelsRect(kSubX, kSubY, kSubW, kSubH);
            EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
            EXPECT_TRUE(sub == CropRect(whole, kSubX, kSubY, kSubW, kSubH))
                << "an FBO sub-rect readback differs from the same rect of its full-extent read in "
                << sub.ByteDiffCount(CropRect(whole, kSubX, kSubY, kSubW, kSubH)) << " bytes";

            BindFbo(m_offscreen);
            ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
            glViewport(kSubX, kSubY, kSubW, kSubH);
            DrawQuadrants();
            glViewport(0, 0, m_offscreen.width, m_offscreen.height);
            const Image placedWhole = ReadPixels(m_offscreen.width, m_offscreen.height);
            EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR));
            EXPECT_EQ(CropRect(placedWhole, kSubX, kSubY, kSubW, kSubH).QuadrantSignature(), kUprightSignature)
                << "an FBO sub-viewport draw must land in GL rows [y0, y0+h) upright";
            EXPECT_TRUE(RegionIsMostly(placedWhole, 0, m_offscreen.width - 1, 0, kSubY - 2, "black", 0.0,
                                       "below an FBO sub-viewport"));
            EXPECT_TRUE(RegionIsMostly(placedWhole, 0, m_offscreen.width - 1, kSubY + kSubH + 1,
                                       m_offscreen.height - 1, "black", 0.0, "above an FBO sub-viewport"));
        }

    } // namespace
} // namespace MGITest
