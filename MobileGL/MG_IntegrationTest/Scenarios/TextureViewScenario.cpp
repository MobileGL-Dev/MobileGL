// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/TextureViewScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// glTextureView (ARB_texture_view / GL 4.6 core 8.18) end to end on both backends.
//
// THE DEFECT. glTextureView was a stub that logged once and returned. That is worse than not
// having the function: MobileGL advertises GL 4.6, so LWJGL resolves a non-null pointer, an
// application's capability check passes, it takes the texture-view path, and the view texture it
// then samples has no storage at all. Nothing errors; the picture is simply wrong. The Better
// Clouds Minecraft mod is exactly this shape - its GLCompat gates `supportsTextureView` on
// `caps.glTextureView != NULL`, which was already true, so it ran its FULL path against a view
// that aliased nothing.
//
// WHAT A VIEW IS, and why a copy cannot stand in for one. A view is a second texture NAME over
// the SAME storage. Two consequences the tests below pin, both of which a copy fails:
//   * writes through either name are visible through the other (CoherencyIsBidirectional), and
//   * the two names carry INDEPENDENT per-texture parameters at the same time - which is the
//     entire point for Better Clouds: one D24S8 image, sampled in ONE shading pass through the
//     parent with DEPTH_STENCIL_TEXTURE_MODE = GL_STENCIL_INDEX and through the view with
//     GL_DEPTH_COMPONENT (BetterCloudsCoveragePipeline below).
//
// MECHANISM PER BACKEND. DirectVulkan: the view resolves to the storage texture's ONE
// TextureResource - one VkImage, one tracked layout, one upload path - and its own VkImageViews
// (sub-range, reinterpreted VkFormat, its own aspect) are cached in alternateSampledViews /
// attachmentViews keyed by the whole window. DirectGLES: the view gets its own ES name minted by
// EXT/OES_texture_view over the storage texture's name, so the driver supplies the aliasing and
// per-name parameters come for free. Without that extension the frontend refuses glTextureView
// with GL_INVALID_OPERATION and withholds GL_ARB_texture_view rather than emulate by copying -
// see NoExtensionSupportIsRefusedRatherThanFaked.
//
// CONTROLS. Every case here would pass on a stub for at least one of its assertions, so each one
// also asserts something the stub cannot produce: a non-zero sampled value, a DIFFERENT value
// through the two names, or a value that changed after a write through the other name.

#include <array>
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
        constexpr int kSize = 64;
        // The lower strip no cloud quad covers, so coverage 0 / depth 0 is asserted too - a
        // uniform image would otherwise pass a test that only ever looked at covered texels.
        constexpr int kUncoveredTop = 16;

        constexpr const char* kQuadVertexSource = R"(#version 330 core
in vec2 aPos;
uniform vec4 uRect;   // x0, y0, x1, y1 in NDC
uniform float uDepth; // NDC z
void main() {
    vec2 p = mix(uRect.xy, uRect.zw, aPos);
    gl_Position = vec4(p, uDepth, 1.0);
}
)";

        // Mirrors betterclouds_coverage.fsh's shape: a second fragment output at location 1 whose
        // draw buffer is GL_NONE. The mod declares and writes it while glDrawBuffers names only
        // COLOR_ATTACHMENT0, so a layer that mishandles a write to a NONE draw buffer would either
        // error or clobber attachment 0.
        constexpr const char* kCoverageFragmentSource = R"(#version 330 core
layout (location = 0) out vec4 outColor;
layout (location = 1) out float outUnused;
void main() {
    outColor = vec4(1.0, 0.0, 0.0, 1.0);
    outUnused = 1.0 / 255.0;
}
)";

        // The Better Clouds shading pass, reduced to its sampling. Both fetches name the SAME
        // D24S8 image through two GL texture names bound to two units in this one invocation.
        // `ivec2(gl_FragCoord)` (a truncating vec4 -> ivec2 constructor) is the mod's own spelling
        // at betterclouds_shading.fsh:56, kept verbatim because a strict GLSL front end can reject
        // it; the depth fetch uses the conventional `.xy` form the mod uses at line 117.
        constexpr const char* kShadingFragmentSource = R"(#version 330 core
uniform usampler2D uCoverage;  // the PARENT, DEPTH_STENCIL_TEXTURE_MODE = GL_STENCIL_INDEX
uniform sampler2D uDepthView;  // the VIEW,   DEPTH_STENCIL_TEXTURE_MODE = GL_DEPTH_COMPONENT
out vec4 outColor;
void main() {
    uint coverage = texelFetch(uCoverage, ivec2(gl_FragCoord), 0).r;
    float depth = texelFetch(uDepthView, ivec2(gl_FragCoord.xy), 0).r;
    outColor = vec4(float(coverage) * 0.25, depth, 0.0, 1.0);
    gl_FragDepth = depth;
}
)";

        // Reads a reinterpreting view (GL_R32UI over GL_RGBA8 storage - both VIEW_CLASS_32_BITS)
        // and unpacks the word back into the four bytes it was written as.
        constexpr const char* kDecodeWordFragmentSource = R"(#version 330 core
uniform usampler2D uWords;
out vec4 outColor;
void main() {
    uint word = texelFetch(uWords, ivec2(gl_FragCoord.xy), 0).r;
    outColor = vec4(float((word      ) & 0xFFu) / 255.0,
                    float((word >>  8) & 0xFFu) / 255.0,
                    float((word >> 16) & 0xFFu) / 255.0,
                    float((word >> 24) & 0xFFu) / 255.0);
}
)";

        constexpr const char* kSampleFragmentSource = R"(#version 330 core
uniform sampler2D uTexture;
uniform float uLod;
out vec4 outColor;
void main() {
    outColor = textureLod(uTexture, gl_FragCoord.xy / 64.0, uLod);
}
)";

        std::string Describe(const Rgba8& c) {
            return "rgba(" + std::to_string(c.r) + "," + std::to_string(c.g) + "," + std::to_string(c.b) + "," +
                   std::to_string(c.a) + ")";
        }

        class TextureViewScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                if (!TextureViewUsable()) {
                    GTEST_SKIP() << "glTextureView is unavailable on backend " << Gl().BackendName()
                                 << " (GL_ARB_texture_view not advertised)";
                }
            }

            void TearDown() override {
                if (!Ready()) return;
                for (const GLuint texture : m_textures) {
                    glDeleteTextures(1, &texture);
                }
                m_textures.clear();
                for (const GLuint fbo : m_fbos) {
                    glBindFramebuffer(GL_FRAMEBUFFER, 0);
                    glDeleteFramebuffers(1, &fbo);
                }
                m_fbos.clear();
                for (const GLuint rbo : m_rbos) {
                    glDeleteRenderbuffers(1, &rbo);
                }
                m_rbos.clear();
                for (const GLuint program : m_programs) {
                    glDeleteProgram(program);
                }
                m_programs.clear();
                if (m_vao != 0) {
                    glBindVertexArray(0);
                    glDeleteVertexArrays(1, &m_vao);
                    m_vao = 0;
                }
                if (m_vbo != 0) {
                    glDeleteBuffers(1, &m_vbo);
                    m_vbo = 0;
                }
            }

            // A trivial same-format full-range view. It exercises nothing the cases below test,
            // so a backend that simply does not have the feature skips instead of failing every
            // one of them - the same shape CopyImageLayeredScenario uses for glCopyImageSubData.
            bool TextureViewUsable() {
                GLuint storage = 0;
                glGenTextures(1, &storage);
                glBindTexture(GL_TEXTURE_2D, storage);
                glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 1, 1);
                glBindTexture(GL_TEXTURE_2D, 0);
                GLuint view = 0;
                glGenTextures(1, &view);
                while (glGetError() != GL_NO_ERROR) {
                }
                glTextureView(view, GL_TEXTURE_2D, storage, GL_RGBA8, 0, 1, 0, 1);
                const bool usable = glGetError() == GL_NO_ERROR;
                glDeleteTextures(1, &view);
                glDeleteTextures(1, &storage);
                return usable;
            }

            GLuint MakeVao() {
                if (m_vao != 0) return m_vao;
                // A unit quad; the vertex shader maps it onto whatever NDC rect uRect names, so
                // one buffer serves every draw here.
                static constexpr float kQuad[] = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f,
                                                  1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
                glGenVertexArrays(1, &m_vao);
                glBindVertexArray(m_vao);
                glGenBuffers(1, &m_vbo);
                glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
                glBufferData(GL_ARRAY_BUFFER, sizeof(kQuad), kQuad, GL_STATIC_DRAW);
                glEnableVertexAttribArray(0);
                glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
                return m_vao;
            }

            GLuint MakeProgram(const char* vertexSource, const char* fragmentSource) {
                std::string error;
                const GLuint program = CompileProgram(vertexSource, fragmentSource, &error);
                EXPECT_NE(program, 0u) << "program failed to build: " << error;
                if (program != 0) m_programs.push_back(program);
                return program;
            }

            GLuint MakeTexture() {
                GLuint texture = 0;
                glGenTextures(1, &texture);
                m_textures.push_back(texture);
                return texture;
            }

            GLuint MakeFbo() {
                GLuint fbo = 0;
                glGenFramebuffers(1, &fbo);
                m_fbos.push_back(fbo);
                return fbo;
            }

            // A 2D texture with immutable storage and NEAREST filtering, i.e. what every case
            // here views. Levels beyond 1 stay undefined until a caller fills them.
            GLuint MakeImmutable2D(GLenum internalFormat, int levels, int width, int height) {
                const GLuint texture = MakeTexture();
                glBindTexture(GL_TEXTURE_2D, texture);
                glTexStorage2D(GL_TEXTURE_2D, levels, internalFormat, width, height);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                return texture;
            }

            void DrawQuad(GLuint program, float x0, float y0, float x1, float y1, float depth) {
                glUseProgram(program);
                glUniform4f(glGetUniformLocation(program, "uRect"), x0, y0, x1, y1);
                const GLint depthLocation = glGetUniformLocation(program, "uDepth");
                if (depthLocation >= 0) glUniform1f(depthLocation, depth);
                glBindVertexArray(MakeVao());
                glDrawArrays(GL_TRIANGLES, 0, 6);
            }

            // Reads the colour texture currently attached to `fbo` as COLOR_ATTACHMENT0.
            Image ReadFbo(GLuint fbo, int width, int height) {
                glBindFramebuffer(GL_FRAMEBUFFER, fbo);
                glReadBuffer(GL_COLOR_ATTACHMENT0);
                return ReadPixels(width, height);
            }

            // Every pixel of the inclusive region must match `expected` within `tolerance` per
            // channel. Whole-region rather than a spot check, for the reason HeadlessGL.h gives:
            // three of four vertices carrying stale data still paints a correct centre pixel.
            void ExpectRegion(const Image& image, int x0, int x1, int y0, int y1, Rgba8 expected, int tolerance,
                              const char* what) {
                int offenders = 0;
                Rgba8 firstOffender{};
                int firstX = -1;
                int firstY = -1;
                for (int y = y0; y <= y1; ++y) {
                    for (int x = x0; x <= x1; ++x) {
                        const Rgba8 actual = image.At(x, y);
                        const bool ok = std::abs(int(actual.r) - int(expected.r)) <= tolerance &&
                                        std::abs(int(actual.g) - int(expected.g)) <= tolerance &&
                                        std::abs(int(actual.b) - int(expected.b)) <= tolerance &&
                                        std::abs(int(actual.a) - int(expected.a)) <= tolerance;
                        if (!ok) {
                            if (offenders == 0) {
                                firstOffender = actual;
                                firstX = x;
                                firstY = y;
                            }
                            ++offenders;
                        }
                    }
                }
                EXPECT_EQ(offenders, 0) << what << ": " << offenders << " of "
                                        << (x1 - x0 + 1) * (y1 - y0 + 1) << " pixels disagree; first at (" << firstX
                                        << ", " << firstY << ") is " << Describe(firstOffender) << ", expected "
                                        << Describe(expected) << " +/- " << tolerance;
            }

            std::vector<GLuint> m_textures;
            std::vector<GLuint> m_fbos;
            std::vector<GLuint> m_rbos;
            std::vector<GLuint> m_programs;
            GLuint m_vao = 0;
            GLuint m_vbo = 0;
        };

        // ------------------------------------------------------------------------------------
        // The driving case: the Better Clouds full-mode pipeline, in its real order.
        // ------------------------------------------------------------------------------------
        TEST_F(TextureViewScenario, BetterCloudsCoveragePipeline) {
            if (!Ready() || IsSkipped()) return;

            // --- Resources.java:230-251, in order ---------------------------------------------
            const GLuint coverageColor = MakeImmutable2D(GL_RGBA8, 1, kSize, kSize);
            const GLuint coverage = MakeTexture();
            glBindTexture(GL_TEXTURE_2D, coverage);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_DEPTH24_STENCIL8, kSize, kSize);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_DEPTH_STENCIL_TEXTURE_MODE, GL_STENCIL_INDEX);

            const GLuint coverageFbo = MakeFbo();
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, coverageFbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, coverageColor, 0);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, coverage, 0);
            const GLenum drawBuffers[] = {GL_COLOR_ATTACHMENT0};
            glDrawBuffers(1, drawBuffers);
            ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE))
                << "the coverage framebuffer is incomplete; the mod would silently demote to its "
                   "fallback configuration here (Resources.java:187-209)";

            // The view is made from a name glGenTextures has only RESERVED - it has never been
            // bound, so glTextureView has to instantiate the texture object itself.
            const GLuint coverageDepthView = MakeTexture();
            glTextureView(coverageDepthView, GL_TEXTURE_2D, coverage, GL_DEPTH24_STENCIL8, 0, 1, 0, 1);
            ASSERT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR)) << "glTextureView raised an error";
            glBindTexture(GL_TEXTURE_2D, coverageDepthView);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_DEPTH_STENCIL_TEXTURE_MODE, GL_DEPTH_COMPONENT);
            glBindTexture(GL_TEXTURE_2D, 0);
            ASSERT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR)) << "setting up the view raised an error";

            // The two names must be distinguishable through the queries, or nothing below proves
            // which one produced a sample.
            GLint parentMode = 0;
            GLint viewMode = 0;
            glBindTexture(GL_TEXTURE_2D, coverage);
            glGetTexParameteriv(GL_TEXTURE_2D, GL_DEPTH_STENCIL_TEXTURE_MODE, &parentMode);
            glBindTexture(GL_TEXTURE_2D, coverageDepthView);
            glGetTexParameteriv(GL_TEXTURE_2D, GL_DEPTH_STENCIL_TEXTURE_MODE, &viewMode);
            glBindTexture(GL_TEXTURE_2D, 0);
            EXPECT_EQ(parentMode, GL_STENCIL_INDEX) << "the parent must keep the stencil aspect";
            EXPECT_EQ(viewMode, GL_DEPTH_COMPONENT)
                << "the view must carry its OWN depth-stencil mode; sharing one parameter set with "
                   "the parent is precisely what a texture view exists to avoid";

            // --- OpenGLRenderer.java:244-344, the coverage pass -------------------------------
            const GLuint coverageProgram = MakeProgram(kQuadVertexSource, kCoverageFragmentSource);
            ASSERT_NE(coverageProgram, 0u);

            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, coverageFbo);
            glViewport(0, 0, kSize, kSize);
            glEnable(GL_DEPTH_TEST);
            glDepthMask(GL_TRUE);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            // Reverse-Z, as the mod runs it (OpenGLRenderer.java:236/241).
            glClearDepth(0.0);
            glDepthFunc(GL_GEQUAL);
            glDisable(GL_BLEND);
            glEnable(GL_STENCIL_TEST);
            glStencilMask(0xff);
            glClearStencil(0);
            // The coverage COUNT: one increment per depth-passing cloud fragment.
            glStencilOp(GL_KEEP, GL_INCR, GL_INCR);
            glStencilFunc(GL_ALWAYS, 0xff, 0xff);
            glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
            glClear(GL_STENCIL_BUFFER_BIT | GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

            // Quad 1 covers everything above the uncovered strip, at window depth 0.25.
            const float stripTop = 2.0f * (float(kUncoveredTop) / float(kSize)) - 1.0f;
            DrawQuad(coverageProgram, -1.0f, stripTop, 1.0f, 1.0f, -0.5f);
            // Quad 2 covers the right half of that, at window depth 0.75 - nearer under GEQUAL,
            // so it both passes the depth test and increments the stencil a second time.
            DrawQuad(coverageProgram, 0.0f, stripTop, 1.0f, 1.0f, 0.5f);
            ASSERT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR)) << "the coverage pass raised an error";

            // --- OpenGLRenderer.java:393-464, the shading pass --------------------------------
            // A different draw framebuffer, exactly as the mod does (it hands the frame back to
            // Blaze3D before shading). The coverage texture stays ATTACHED to coverageFbo while
            // being sampled here, which is the shape a lazy/deferred FBO binding gets wrong.
            ColorFbo destination = MakeColorFbo(kSize, kSize);
            ASSERT_NE(destination.fbo, 0u);
            GLuint destinationDepth = 0;
            glGenRenderbuffers(1, &destinationDepth);
            m_rbos.push_back(destinationDepth);
            glBindRenderbuffer(GL_RENDERBUFFER, destinationDepth);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, kSize, kSize);
            glBindFramebuffer(GL_FRAMEBUFFER, destination.fbo);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, destinationDepth);
            ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));

            glViewport(0, 0, kSize, kSize);
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClearDepth(0.0);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            glDepthFunc(GL_GEQUAL);
            glDepthMask(GL_TRUE);
            glEnable(GL_DEPTH_TEST);
            glDisable(GL_STENCIL_TEST);
            // The mod's own indexed/non-indexed colour-mask pair (OpenGLRenderer.java:411-412).
            glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
            glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

            const GLuint shadingProgram = MakeProgram(kQuadVertexSource, kShadingFragmentSource);
            ASSERT_NE(shadingProgram, 0u);
            glUseProgram(shadingProgram);
            // Unit 1 = the view (depth aspect), unit 3 = the parent (stencil aspect), the mod's
            // own unit assignment (Resources.java:309/311).
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, coverageDepthView);
            glActiveTexture(GL_TEXTURE3);
            glBindTexture(GL_TEXTURE_2D, coverage);
            glUniform1i(glGetUniformLocation(shadingProgram, "uDepthView"), 1);
            glUniform1i(glGetUniformLocation(shadingProgram, "uCoverage"), 3);
            glActiveTexture(GL_TEXTURE0);

            DrawQuad(shadingProgram, -1.0f, -1.0f, 1.0f, 1.0f, 0.0f);
            ASSERT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR)) << "the shading pass raised an error";

            const Image shaded = ReadFbo(destination.fbo, kSize, kSize);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

            // R = coverage * 0.25 (so 1 -> 64, 2 -> 128), G = the depth read THROUGH THE VIEW.
            // A stub view samples (0,0,0,1), which fails the green channel of both covered
            // regions; a view that inherited the parent's stencil aspect fails them too.
            constexpr int kTolerance = 3;
            ExpectRegion(shaded, 1, kSize - 2, 1, kUncoveredTop - 2, Rgba8{0, 0, 0, 255}, kTolerance,
                         "the uncovered strip must read coverage 0 and cleared depth 0");
            ExpectRegion(shaded, 1, kSize / 2 - 2, kUncoveredTop + 1, kSize - 2, Rgba8{64, 64, 0, 255}, kTolerance,
                         "one cloud quad: stencil 1 through the parent, window depth 0.25 through the view");
            ExpectRegion(shaded, kSize / 2 + 1, kSize - 2, kUncoveredTop + 1, kSize - 2, Rgba8{128, 191, 0, 255},
                         kTolerance,
                         "two overlapping cloud quads: stencil 2 through the parent, window depth 0.75 "
                         "through the view");

            DestroyColorFbo(destination);
        }

        // ------------------------------------------------------------------------------------
        // Storage sharing, in both directions. This is the assertion a copy-based emulation
        // fails, and the reason the no-EXT path refuses rather than emulates.
        // ------------------------------------------------------------------------------------
        TEST_F(TextureViewScenario, CoherencyIsBidirectional) {
            if (!Ready() || IsSkipped()) return;

            const GLuint storage = MakeImmutable2D(GL_RGBA8, 1, kSize, kSize);
            const GLuint view = MakeTexture();
            glTextureView(view, GL_TEXTURE_2D, storage, GL_RGBA8, 0, 1, 0, 1);
            ASSERT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));

            // Render red through the PARENT's name...
            const GLuint fbo = MakeFbo();
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, storage, 0);
            ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));
            glViewport(0, 0, kSize, kSize);
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_STENCIL_TEST);
            glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);

            // ...and read it back through the VIEW's.
            const GLuint viewFbo = MakeFbo();
            glBindFramebuffer(GL_FRAMEBUFFER, viewFbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, view, 0);
            ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE))
                << "a texture view must be attachable like any other texture";
            Image throughView = ReadFbo(viewFbo, kSize, kSize);
            ExpectRegion(throughView, 0, kSize - 1, 0, kSize - 1, Rgba8{255, 0, 0, 255}, 1,
                         "a write through the parent must be visible through the view");

            // Now the other direction: write green through the VIEW, read through the PARENT.
            glBindFramebuffer(GL_FRAMEBUFFER, viewFbo);
            glClearColor(0.0f, 1.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            const Image throughParent = ReadFbo(fbo, kSize, kSize);
            ExpectRegion(throughParent, 0, kSize - 1, 0, kSize - 1, Rgba8{0, 255, 0, 255}, 1,
                         "a write through the view must be visible through the parent - they are one "
                         "storage, not two");
        }

        // ------------------------------------------------------------------------------------
        // Format reinterpretation within a view class (GL 4.6 core table 8.21).
        // ------------------------------------------------------------------------------------
        TEST_F(TextureViewScenario, ReinterpretingViewReadsTheSameBitsThroughAnotherFormat) {
            if (!Ready() || IsSkipped()) return;

            // GL_RGBA8 and GL_R32UI are both VIEW_CLASS_32_BITS, so one may be viewed as the
            // other. Filling the RGBA8 storage with a known byte pattern makes the R32UI view's
            // answer a fact about the BITS rather than about the colour.
            const GLuint storage = MakeImmutable2D(GL_RGBA8, 1, kSize, kSize);
            std::vector<std::uint8_t> texels(static_cast<std::size_t>(kSize) * kSize * 4);
            for (std::size_t i = 0; i < texels.size(); i += 4) {
                texels[i + 0] = 0x40;
                texels[i + 1] = 0x80;
                texels[i + 2] = 0xC0;
                texels[i + 3] = 0xFF;
            }
            glBindTexture(GL_TEXTURE_2D, storage);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kSize, kSize, GL_RGBA, GL_UNSIGNED_BYTE, texels.data());
            glBindTexture(GL_TEXTURE_2D, 0);
            ASSERT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR)) << "seeding the storage raised an error";

            // NEGATIVE CONTROL. Everything below reads the storage through a REINTERPRETING view,
            // so a test that only asserted the view's answer could not tell "the reinterpret is
            // wrong" from "the seed never reached the GPU at all". Read the same texels through
            // the parent's own format first.
            const GLuint parentFbo = MakeFbo();
            glBindFramebuffer(GL_FRAMEBUFFER, parentFbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, storage, 0);
            ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));
            const Image seeded = ReadFbo(parentFbo, kSize, kSize);
            ExpectRegion(seeded, 0, kSize - 1, 0, kSize - 1, Rgba8{0x40, 0x80, 0xC0, 0xFF}, 1,
                         "control: the storage must hold the seeded byte pattern before any view reads it");

            const GLuint view = MakeTexture();
            glTextureView(view, GL_TEXTURE_2D, storage, GL_R32UI, 0, 1, 0, 1);
            ASSERT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR)) << "an in-class reinterpret must be accepted";
            glBindTexture(GL_TEXTURE_2D, view);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

            GLint viewFormat = 0;
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &viewFormat);
            glBindTexture(GL_TEXTURE_2D, 0);
            EXPECT_EQ(viewFormat, GL_R32UI) << "the view must report its OWN internal format";

            // A REAL GL_R32UI texture holding the very word the storage's bytes spell. The
            // assertion below is that the view and this texture sample IDENTICALLY.
            //
            // Comparing against a reference texture rather than against a hard-coded colour is
            // deliberate. Sampling a 32-bit integer texture is not itself what this scenario is
            // about, and llvmpipe's ES driver does it inconsistently (verified outside MobileGL,
            // with a raw-EGL program that reproduces the same wrong decode with NO view in play).
            // Holding both sides to the same driver factors that out completely: whatever the
            // driver makes of a usampler2D fetch, the view has to make the same thing of it, or
            // it is not delivering the storage's bits. A view that samples zero, that lands on
            // the wrong texels, or that lost its format still fails.
            constexpr std::uint32_t kExpectedWord = 0xFFC08040u;  // little-endian A,B,G,R
            const GLuint reference = MakeImmutable2D(GL_R32UI, 1, kSize, kSize);
            std::vector<std::uint32_t> words(static_cast<std::size_t>(kSize) * kSize, kExpectedWord);
            glBindTexture(GL_TEXTURE_2D, reference);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kSize, kSize, GL_RED_INTEGER, GL_UNSIGNED_INT, words.data());
            glBindTexture(GL_TEXTURE_2D, 0);
            ASSERT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR)) << "seeding the reference texture failed";

            const GLuint program = MakeProgram(kQuadVertexSource, kDecodeWordFragmentSource);
            ASSERT_NE(program, 0u);

            ColorFbo destination = MakeColorFbo(kSize, kSize);
            ASSERT_NE(destination.fbo, 0u);
            const auto decodeThrough = [&](GLuint texture) {
                glBindFramebuffer(GL_FRAMEBUFFER, destination.fbo);
                glViewport(0, 0, kSize, kSize);
                glDisable(GL_DEPTH_TEST);
                glDisable(GL_STENCIL_TEST);
                glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
                glClear(GL_COLOR_BUFFER_BIT);
                glUseProgram(program);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, texture);
                glUniform1i(glGetUniformLocation(program, "uWords"), 0);
                DrawQuad(program, -1.0f, -1.0f, 1.0f, 1.0f, 0.0f);
                EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR)) << "sampling raised an error";
                return ReadFbo(destination.fbo, kSize, kSize);
            };

            const Image throughReference = decodeThrough(reference);
            const Image throughView = decodeThrough(view);

            // Guard against the degenerate agreement of two black images: the reference must
            // itself carry something, or "identical" would prove nothing.
            const Rgba8 referenceTexel = throughReference.At(kSize / 2, kSize / 2);
            ASSERT_FALSE(referenceTexel == (Rgba8{0, 0, 0, 0}))
                << "the reference GL_R32UI texture sampled as nothing, so the comparison below is vacuous";

            std::size_t mismatches = 0;
            for (int y = 0; y < kSize; ++y) {
                for (int x = 0; x < kSize; ++x) {
                    if (!(throughView.At(x, y) == throughReference.At(x, y))) ++mismatches;
                }
            }
            EXPECT_EQ(mismatches, 0u)
                << "the GL_R32UI view of GL_RGBA8 storage must sample exactly what a real GL_R32UI texture "
                   "holding the same word does; view centre is " << Describe(throughView.At(kSize / 2, kSize / 2))
                << ", reference centre is " << Describe(referenceTexel);
            DestroyColorFbo(destination);
        }

        // ------------------------------------------------------------------------------------
        // Sub-ranges: one mip level of two, and one layer of an array.
        // ------------------------------------------------------------------------------------
        TEST_F(TextureViewScenario, ViewOfOneMipLevelAddressesThatLevelAsItsOwnLevelZero) {
            if (!Ready() || IsSkipped()) return;

            const GLuint storage = MakeImmutable2D(GL_RGBA8, 2, kSize, kSize);
            // Level 0 red, level 1 blue, so the view's answer names the level it opened onto.
            const GLuint seedFbo = MakeFbo();
            glBindFramebuffer(GL_FRAMEBUFFER, seedFbo);
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_STENCIL_TEST);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, storage, 0);
            glViewport(0, 0, kSize, kSize);
            glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, storage, 1);
            glViewport(0, 0, kSize / 2, kSize / 2);
            glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            ASSERT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR)) << "seeding the mip chain raised an error";

            const GLuint view = MakeTexture();
            glTextureView(view, GL_TEXTURE_2D, storage, GL_RGBA8, 1, 1, 0, 1);
            ASSERT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));

            GLint minLevel = -1;
            GLint numLevels = -1;
            GLint immutableLevels = -1;
            glBindTexture(GL_TEXTURE_2D, view);
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_VIEW_MIN_LEVEL, &minLevel);
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_VIEW_NUM_LEVELS, &numLevels);
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_IMMUTABLE_LEVELS, &immutableLevels);
            glBindTexture(GL_TEXTURE_2D, 0);
            EXPECT_EQ(minLevel, 1);
            EXPECT_EQ(numLevels, 1);
            // GL 4.6 core 8.18: inherited from the ORIGINAL, not set to <numlevels>.
            EXPECT_EQ(immutableLevels, 2) << "TEXTURE_IMMUTABLE_LEVELS is the original texture's value";

            // The view's level 0 IS the parent's level 1: attaching level 0 of the view must find
            // the blue half-size image.
            const GLuint viewFbo = MakeFbo();
            glBindFramebuffer(GL_FRAMEBUFFER, viewFbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, view, 0);
            ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));
            const Image levelOne = ReadFbo(viewFbo, kSize / 2, kSize / 2);
            ExpectRegion(levelOne, 0, kSize / 2 - 1, 0, kSize / 2 - 1, Rgba8{0, 0, 255, 255}, 1,
                         "the view's level 0 must be the parent's level 1 (blue), not its level 0 (red)");
        }

        TEST_F(TextureViewScenario, ViewOfOneArrayLayerAddressesThatLayer) {
            if (!Ready() || IsSkipped()) return;

            constexpr int kLayers = 4;
            constexpr int kChosenLayer = 2;
            const GLuint storage = MakeTexture();
            glBindTexture(GL_TEXTURE_2D_ARRAY, storage);
            glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_RGBA8, kSize, kSize, kLayers);
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            ASSERT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));

            // A different colour per layer, so a view that lost its layer offset reads the wrong
            // one rather than merely reading nothing.
            const GLuint seedFbo = MakeFbo();
            glBindFramebuffer(GL_FRAMEBUFFER, seedFbo);
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_STENCIL_TEST);
            glViewport(0, 0, kSize, kSize);
            for (int layer = 0; layer < kLayers; ++layer) {
                glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, storage, 0, layer);
                glClearColor(float(layer) / 8.0f, 1.0f - float(layer) / 8.0f, 0.5f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT);
            }
            ASSERT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR)) << "seeding the array layers raised an error";

            const GLuint view = MakeTexture();
            glTextureView(view, GL_TEXTURE_2D, storage, GL_RGBA8, 0, 1, kChosenLayer, 1);
            ASSERT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR)) << "2D_ARRAY -> 2D is a legal view pair";

            GLint minLayer = -1;
            GLint numLayers = -1;
            glBindTexture(GL_TEXTURE_2D, view);
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_VIEW_MIN_LAYER, &minLayer);
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_VIEW_NUM_LAYERS, &numLayers);
            glBindTexture(GL_TEXTURE_2D, 0);
            EXPECT_EQ(minLayer, kChosenLayer);
            EXPECT_EQ(numLayers, 1);

            const GLuint viewFbo = MakeFbo();
            glBindFramebuffer(GL_FRAMEBUFFER, viewFbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, view, 0);
            ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));
            const Image sliced = ReadFbo(viewFbo, kSize, kSize);
            const Rgba8 expected{static_cast<std::uint8_t>(kChosenLayer * 255 / 8),
                                 static_cast<std::uint8_t>(255 - kChosenLayer * 255 / 8), 128, 255};
            ExpectRegion(sliced, 0, kSize - 1, 0, kSize - 1, expected, 2,
                         "a single-layer 2D view of an array must address the layer it named");
        }

        // ------------------------------------------------------------------------------------
        // Views of views compose; the composed view still reaches the ROOT storage.
        // ------------------------------------------------------------------------------------
        TEST_F(TextureViewScenario, ViewOfAViewComposesTheLevelRanges) {
            if (!Ready() || IsSkipped()) return;

            constexpr int kLevels = 3;
            const GLuint storage = MakeImmutable2D(GL_RGBA8, kLevels, kSize, kSize);
            const GLuint seedFbo = MakeFbo();
            glBindFramebuffer(GL_FRAMEBUFFER, seedFbo);
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_STENCIL_TEST);
            for (int level = 0; level < kLevels; ++level) {
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, storage, level);
                glViewport(0, 0, kSize >> level, kSize >> level);
                glClearColor(0.0f, 0.0f, float(level + 1) / 4.0f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT);
            }
            ASSERT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));

            // First view opens onto levels [1, 3); the second takes level 1 OF THAT, which is the
            // root's level 2. GL 4.6 core 8.18 makes the offsets add.
            const GLuint firstView = MakeTexture();
            glTextureView(firstView, GL_TEXTURE_2D, storage, GL_RGBA8, 1, 2, 0, 1);
            ASSERT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
            const GLuint secondView = MakeTexture();
            glTextureView(secondView, GL_TEXTURE_2D, firstView, GL_RGBA8, 1, 1, 0, 1);
            ASSERT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR)) << "origtexture may itself be a view";

            GLint minLevel = -1;
            GLint numLevels = -1;
            glBindTexture(GL_TEXTURE_2D, secondView);
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_VIEW_MIN_LEVEL, &minLevel);
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_VIEW_NUM_LEVELS, &numLevels);
            glBindTexture(GL_TEXTURE_2D, 0);
            EXPECT_EQ(minLevel, 2) << "TEXTURE_VIEW_MIN_LEVEL adds the original's";
            EXPECT_EQ(numLevels, 1);

            const GLuint viewFbo = MakeFbo();
            glBindFramebuffer(GL_FRAMEBUFFER, viewFbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, secondView, 0);
            ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));
            const Image composed = ReadFbo(viewFbo, kSize >> 2, kSize >> 2);
            ExpectRegion(composed, 0, (kSize >> 2) - 1, 0, (kSize >> 2) - 1, Rgba8{0, 0, 191, 255}, 2,
                         "the composed view must land on the root's level 2");
        }

        // ------------------------------------------------------------------------------------
        // GL name-deletion semantics: the storage outlives the original's NAME.
        // ------------------------------------------------------------------------------------
        TEST_F(TextureViewScenario, DeletingTheOriginalKeepsTheViewUsable) {
            if (!Ready() || IsSkipped()) return;

            GLuint storage = 0;
            glGenTextures(1, &storage);
            glBindTexture(GL_TEXTURE_2D, storage);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, kSize, kSize);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

            const GLuint view = MakeTexture();
            glTextureView(view, GL_TEXTURE_2D, storage, GL_RGBA8, 0, 1, 0, 1);
            ASSERT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));

            const GLuint fbo = MakeFbo();
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, storage, 0);
            glViewport(0, 0, kSize, kSize);
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_STENCIL_TEST);
            glClearColor(0.0f, 1.0f, 1.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);

            // The NAME goes; the storage may not, because a view still references it
            // (GL 4.6 core 5.1.2 - an object is not deleted while anything still refers to it).
            glDeleteTextures(1, &storage);
            EXPECT_EQ(glIsTexture(storage), static_cast<GLboolean>(GL_FALSE));
            ASSERT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));

            // Sample the view through a shader, so the answer comes from a live descriptor rather
            // than from an attachment the frontend might have kept alive by other means.
            ColorFbo destination = MakeColorFbo(kSize, kSize);
            ASSERT_NE(destination.fbo, 0u);
            glBindFramebuffer(GL_FRAMEBUFFER, destination.fbo);
            glViewport(0, 0, kSize, kSize);
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);

            const GLuint program = MakeProgram(kQuadVertexSource, kSampleFragmentSource);
            ASSERT_NE(program, 0u);
            glUseProgram(program);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, view);
            glUniform1i(glGetUniformLocation(program, "uTexture"), 0);
            glUniform1f(glGetUniformLocation(program, "uLod"), 0.0f);
            DrawQuad(program, -1.0f, -1.0f, 1.0f, 1.0f, 0.0f);
            ASSERT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));

            const Image sampled = ReadFbo(destination.fbo, kSize, kSize);
            ExpectRegion(sampled, 1, kSize - 2, 1, kSize - 2, Rgba8{0, 255, 255, 255}, 2,
                         "the view must still reach its storage after the original's name was deleted");
            DestroyColorFbo(destination);
        }
    } // namespace
} // namespace MGITest
