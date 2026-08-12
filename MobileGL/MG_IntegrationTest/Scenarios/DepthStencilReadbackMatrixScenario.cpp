// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/DepthStencilReadbackMatrixScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - THE DEPTH/STENCIL READBACK MATRIX: every verb, every source kind.
//
// DepthStencilReadbackScenario pins the default framebuffer. This file pins the rest of
// the surface a depth/stencil read has to cover, because the three verbs and the four
// source kinds do NOT share a code path by accident - they share one on purpose, and a
// change that quietly serves only one of them is exactly what these assertions catch:
//
//   verbs         glReadPixels(GL_DEPTH_COMPONENT | GL_STENCIL_INDEX | GL_DEPTH_STENCIL),
//                 glGetTexImage(GL_DEPTH_STENCIL), glCopyTexImage2D followed by a read
//   source kinds  depth(-stencil) TEXTURE, RENDERBUFFER (not samplable at all),
//                 MULTISAMPLE renderbuffer (needs a resolve first), default framebuffer
//   formats       DEPTH24_STENCIL8, DEPTH32F_STENCIL8, DEPTH_COMPONENT16/24/32F,
//                 STENCIL_INDEX8
//   client types  GL_FLOAT / GL_UNSIGNED_INT / GL_UNSIGNED_SHORT depth, GL_INT /
//                 GL_UNSIGNED_BYTE stencil, both packed GL_DEPTH_STENCIL layouts
//
// On DirectGLES none of this exists natively - ES has no depth or stencil readback in
// core - so every assertion here is really an assertion about the shader-sampling
// emulation. The catch is that some ES drivers accept the reads anyway (Mesa does,
// Adreno does not), which would make the emulation dead code on the very stack the
// headless suite runs on. That is what the second ctest registration is for: the same
// scenarios run again with MOBILEGL_ESPRYT_FORCE_DS_READBACK_EMULATION=1, which takes the
// native spellings off the table and leaves only the path the device actually uses.
//
// Every destination is poisoned with a value the correct answer cannot be, so "the
// backend wrote nothing" fails loudly instead of passing on a coincidence - a test that
// only checked "no GL error" would pass against a readback that never touched the buffer,
// which is precisely how this whole cluster hid for so long.

#include <cmath>
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

        constexpr float kDepthPoison = 0.2f;
        constexpr int kStencilPoison = 50;
        constexpr int kWidth = 64;
        constexpr int kHeight = 48;

        // A depth-stencil pair no clear in these tests produces, packed both ways.
        constexpr unsigned int kPacked24_8Poison = 0xAAAAAA33u;

        struct D32fS8 {
            float depth;
            unsigned int stencil;
        };

        // Everything a source needs to be read: the framebuffer to bind, plus the objects
        // to delete afterwards.
        struct DepthSource {
            GLuint fbo = 0;
            GLuint colorTexture = 0;
            GLuint depthTexture = 0;
            GLuint depthRenderbuffer = 0;
            GLuint colorRenderbuffer = 0;
        };

        void DestroySource(DepthSource& source) {
            if (source.fbo != 0) glDeleteFramebuffers(1, &source.fbo);
            if (source.colorTexture != 0) glDeleteTextures(1, &source.colorTexture);
            if (source.depthTexture != 0) glDeleteTextures(1, &source.depthTexture);
            if (source.depthRenderbuffer != 0) glDeleteRenderbuffers(1, &source.depthRenderbuffer);
            if (source.colorRenderbuffer != 0) glDeleteRenderbuffers(1, &source.colorRenderbuffer);
            source = DepthSource{};
        }

        GLenum AttachmentPointFor(GLenum internalFormat) {
            switch (internalFormat) {
            case GL_DEPTH24_STENCIL8:
            case GL_DEPTH32F_STENCIL8: return GL_DEPTH_STENCIL_ATTACHMENT;
            case GL_STENCIL_INDEX8: return GL_STENCIL_ATTACHMENT;
            default: return GL_DEPTH_ATTACHMENT;
            }
        }

        bool FormatHasDepth(GLenum internalFormat) { return internalFormat != GL_STENCIL_INDEX8; }
        bool FormatHasStencil(GLenum internalFormat) {
            return internalFormat == GL_DEPTH24_STENCIL8 || internalFormat == GL_DEPTH32F_STENCIL8 ||
                   internalFormat == GL_STENCIL_INDEX8;
        }

        // A framebuffer whose depth/stencil lives in a TEXTURE. The colour attachment is
        // there so a stencil-only or depth-only framebuffer still has something to size it.
        DepthSource MakeTextureSource(GLenum internalFormat) {
            DepthSource source;
            glGenFramebuffers(1, &source.fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, source.fbo);
            glGenTextures(1, &source.colorTexture);
            glBindTexture(GL_TEXTURE_2D, source.colorTexture);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, kWidth, kHeight);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, source.colorTexture, 0);
            glGenTextures(1, &source.depthTexture);
            glBindTexture(GL_TEXTURE_2D, source.depthTexture);
            glTexStorage2D(GL_TEXTURE_2D, 1, internalFormat, kWidth, kHeight);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glFramebufferTexture2D(GL_FRAMEBUFFER, AttachmentPointFor(internalFormat), GL_TEXTURE_2D,
                                   source.depthTexture, 0);
            return source;
        }

        // The same, with the depth/stencil in a RENDERBUFFER - which cannot be sampled at
        // all, so the readback has no choice but to copy it somewhere samplable first.
        // `samples` > 0 makes it multisample, which additionally needs a resolve.
        DepthSource MakeRenderbufferSource(GLenum internalFormat, int samples) {
            DepthSource source;
            glGenFramebuffers(1, &source.fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, source.fbo);
            glGenRenderbuffers(1, &source.colorRenderbuffer);
            glBindRenderbuffer(GL_RENDERBUFFER, source.colorRenderbuffer);
            if (samples > 0) {
                glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA8, kWidth, kHeight);
            } else {
                glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, kWidth, kHeight);
            }
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, source.colorRenderbuffer);
            glGenRenderbuffers(1, &source.depthRenderbuffer);
            glBindRenderbuffer(GL_RENDERBUFFER, source.depthRenderbuffer);
            if (samples > 0) {
                glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, internalFormat, kWidth, kHeight);
            } else {
                glRenderbufferStorage(GL_RENDERBUFFER, internalFormat, kWidth, kHeight);
            }
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, AttachmentPointFor(internalFormat), GL_RENDERBUFFER,
                                      source.depthRenderbuffer);
            glBindRenderbuffer(GL_RENDERBUFFER, 0);
            return source;
        }

        // Clears the bound framebuffer's depth and stencil to known values, with the masks
        // and the scissor explicitly out of the way (a leaked scissor from an earlier
        // scenario would clip the clear and every assertion after it).
        void ClearDepthStencil(GLenum internalFormat, float depth, int stencil) {
            glDisable(GL_SCISSOR_TEST);
            glViewport(0, 0, kWidth, kHeight);
            GLbitfield mask = 0;
            if (FormatHasDepth(internalFormat)) {
                glDepthMask(GL_TRUE);
                glClearDepth(depth);
                mask |= GL_DEPTH_BUFFER_BIT;
            }
            if (FormatHasStencil(internalFormat)) {
                glStencilMask(0xFFu);
                glClearStencil(stencil);
                mask |= GL_STENCIL_BUFFER_BIT;
            }
            glClear(mask);
        }

        class DepthStencilReadbackMatrixScenario : public ScenarioTest {
        protected:
            // Not every ES driver can render to every depth format (DEPTH_COMPONENT32F and
            // the multisample counts in particular), and an incomplete framebuffer would
            // turn a legitimate "this machine cannot host the source" into a spurious
            // failure about the readback.
            static bool SourceIsUsable() {
                return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GLenum(GL_FRAMEBUFFER_COMPLETE);
            }

            static std::vector<float> ReadDepthFloat(int x, int y, int width, int height) {
                std::vector<float> depth(static_cast<size_t>(width) * height, kDepthPoison);
                glReadPixels(x, y, width, height, GL_DEPTH_COMPONENT, GL_FLOAT, depth.data());
                return depth;
            }

            static std::vector<int> ReadStencilInt(int x, int y, int width, int height) {
                std::vector<int> stencil(static_cast<size_t>(width) * height, kStencilPoison);
                glReadPixels(x, y, width, height, GL_STENCIL_INDEX, GL_INT, stencil.data());
                return stencil;
            }

            // "every value in the region is `expected`" rather than "the middle pixel is":
            // a staging blit that lands the wrong rectangle, or a conversion pass with a
            // half-texel offset, still gets the centre right.
            static void ExpectAllDepth(const std::vector<float>& values, float expected, const char* what) {
                size_t bad = 0;
                float worst = expected;
                for (float value : values) {
                    if (std::fabs(value - expected) > 1.0f / 4096.0f) {
                        if (bad == 0) worst = value;
                        ++bad;
                    }
                }
                EXPECT_EQ(bad, 0u) << what << ": " << bad << " of " << values.size()
                                   << " depth values differ from " << expected << "; first bad value " << worst
                                   << (std::fabs(worst - kDepthPoison) < 1e-6f
                                           ? " - which is the poison value, so nothing was written at all"
                                           : "");
            }

            static void ExpectAllStencil(const std::vector<int>& values, int expected, const char* what) {
                size_t bad = 0;
                int worst = expected;
                for (int value : values) {
                    if (value != expected) {
                        if (bad == 0) worst = value;
                        ++bad;
                    }
                }
                EXPECT_EQ(bad, 0u) << what << ": " << bad << " of " << values.size()
                                   << " stencil values differ from " << expected << "; first bad value " << worst
                                   << (worst == kStencilPoison
                                           ? " - which is the poison value, so nothing was written at all"
                                           : "");
            }
        };

        // ---- glReadPixels across the source kinds -----------------------------------

        struct SourceCase {
            const char* name;
            GLenum internalFormat;
            int samples;
            bool renderbuffer;
        };

        const SourceCase kSourceCases[] = {
            {"texture depth24_stencil8", GL_DEPTH24_STENCIL8, 0, false},
            {"texture depth32f_stencil8", GL_DEPTH32F_STENCIL8, 0, false},
            {"texture depth_component16", GL_DEPTH_COMPONENT16, 0, false},
            {"texture depth_component24", GL_DEPTH_COMPONENT24, 0, false},
            {"texture depth_component32f", GL_DEPTH_COMPONENT32F, 0, false},
            {"renderbuffer depth24_stencil8", GL_DEPTH24_STENCIL8, 0, true},
            {"renderbuffer depth_component24", GL_DEPTH_COMPONENT24, 0, true},
            {"renderbuffer stencil_index8", GL_STENCIL_INDEX8, 0, true},
        };

    } // namespace

    TEST_F(DepthStencilReadbackMatrixScenario, EverySourceKindReadsItsClearBack) {
        if (!Ready()) return;
        int exercised = 0;
        for (const SourceCase& testCase : kSourceCases) {
            SCOPED_TRACE(testCase.name);
            DepthSource source = testCase.renderbuffer
                                     ? MakeRenderbufferSource(testCase.internalFormat, testCase.samples)
                                     : MakeTextureSource(testCase.internalFormat);
            if (!SourceIsUsable()) {
                DestroySource(source);
                continue;
            }
            FirstGLError(); // the storage calls above may have probed an unsupported combination
            ClearDepthStencil(testCase.internalFormat, 0.625f, 9);
            EXPECT_EQ(FirstGLError(), 0u) << "clearing the source";

            if (FormatHasDepth(testCase.internalFormat)) {
                const std::vector<float> depth = ReadDepthFloat(0, 0, kWidth, kHeight);
                EXPECT_EQ(FirstGLError(), 0u) << "glReadPixels(GL_DEPTH_COMPONENT, GL_FLOAT)";
                ExpectAllDepth(depth, 0.625f, testCase.name);
            }
            if (FormatHasStencil(testCase.internalFormat)) {
                const std::vector<int> stencil = ReadStencilInt(0, 0, kWidth, kHeight);
                EXPECT_EQ(FirstGLError(), 0u) << "glReadPixels(GL_STENCIL_INDEX, GL_INT)";
                ExpectAllStencil(stencil, 9, testCase.name);
            }
            ++exercised;
            DestroySource(source);
        }
        // A machine that hosted none of the sources would report a vacuous pass.
        EXPECT_GE(exercised, 4) << "too few depth/stencil source kinds were usable to call this a matrix";
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        Gl().EndFrame();
    }

    // Depth and stencil in two SEPARATE objects, with two different formats, on the same
    // framebuffer. Legal GL, and the shape KHR-GL3x.framebuffer_blit builds when its depth
    // config and its stencil config are configured independently - so a readback that
    // describes "the" depth/stencil source as one thing serves whichever aspect it happened
    // to find first and silently abandons the other. Each aspect has to be staged from its
    // own attachment, in its own format.
    TEST_F(DepthStencilReadbackMatrixScenario, SeparateDepthAndStencilAttachmentsAreBothReadable) {
        if (!Ready()) return;
        DepthSource source;
        glGenFramebuffers(1, &source.fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, source.fbo);
        glGenRenderbuffers(1, &source.colorRenderbuffer);
        glBindRenderbuffer(GL_RENDERBUFFER, source.colorRenderbuffer);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, kWidth, kHeight);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, source.colorRenderbuffer);
        // Depth in a DEPTH_COMPONENT24 renderbuffer...
        glGenRenderbuffers(1, &source.depthRenderbuffer);
        glBindRenderbuffer(GL_RENDERBUFFER, source.depthRenderbuffer);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, kWidth, kHeight);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, source.depthRenderbuffer);
        // ...and stencil in a STENCIL_INDEX8 one of its own.
        GLuint stencilRenderbuffer = 0;
        glGenRenderbuffers(1, &stencilRenderbuffer);
        glBindRenderbuffer(GL_RENDERBUFFER, stencilRenderbuffer);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_STENCIL_INDEX8, kWidth, kHeight);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, stencilRenderbuffer);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);
        if (!SourceIsUsable()) {
            // Separate depth and stencil images are legal GL but many stacks answer
            // GL_FRAMEBUFFER_UNSUPPORTED for them; say which, so a skip here is a fact about
            // the driver rather than an unexplained hole in the matrix.
            const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            glDeleteRenderbuffers(1, &stencilRenderbuffer);
            DestroySource(source);
            GTEST_SKIP() << "this driver cannot host separate DEPTH_COMPONENT24 and STENCIL_INDEX8 attachments: "
                         << "glCheckFramebufferStatus = 0x" << std::hex << status;
        }
        FirstGLError();

        glDisable(GL_SCISSOR_TEST);
        glViewport(0, 0, kWidth, kHeight);
        glDepthMask(GL_TRUE);
        glStencilMask(0xFFu);
        glClearDepth(0.3125);
        glClearStencil(17);
        glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        ASSERT_EQ(FirstGLError(), 0u);

        const std::vector<float> depth = ReadDepthFloat(0, 0, kWidth, kHeight);
        EXPECT_EQ(FirstGLError(), 0u) << "reading depth from a separately-attached DEPTH_COMPONENT24";
        ExpectAllDepth(depth, 0.3125f, "separate depth attachment");

        const std::vector<int> stencil = ReadStencilInt(0, 0, kWidth, kHeight);
        EXPECT_EQ(FirstGLError(), 0u) << "reading stencil from a separately-attached STENCIL_INDEX8";
        ExpectAllStencil(stencil, 17, "separate stencil attachment");

        glDeleteRenderbuffers(1, &stencilRenderbuffer);
        DestroySource(source);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        Gl().EndFrame();
    }

    // A multisample source is never read directly - glReadPixels on a multisampled
    // framebuffer is INVALID_OPERATION in GL as much as in ES, and the state layer says so.
    // The way multisample depth reaches a reader is a resolve blit into a single-sampled
    // framebuffer, which is then read; that pair is
    // KHR-GL3x.framebuffer_blit.multisampled_to_singlesampled_blit_depth_config_test, and
    // the assertion here is that the resolved depth arrives intact rather than as the
    // destination's own clear value.
    TEST_F(DepthStencilReadbackMatrixScenario, AResolvedMultisampleDepthReadsBackFromTheDestination) {
        if (!Ready()) return;
        DepthSource multisampled = MakeRenderbufferSource(GL_DEPTH24_STENCIL8, 4);
        if (!SourceIsUsable()) {
            DestroySource(multisampled);
            GTEST_SKIP() << "this driver cannot host a 4x multisample DEPTH24_STENCIL8 renderbuffer";
        }
        FirstGLError();
        ClearDepthStencil(GL_DEPTH24_STENCIL8, 0.875f, 0);
        ASSERT_EQ(FirstGLError(), 0u);

        // The destination starts at a depth the resolve must overwrite everywhere.
        DepthSource resolved = MakeTextureSource(GL_DEPTH24_STENCIL8);
        ASSERT_TRUE(SourceIsUsable());
        ClearDepthStencil(GL_DEPTH24_STENCIL8, 0.125f, 0);
        ASSERT_EQ(FirstGLError(), 0u);

        glBindFramebuffer(GL_READ_FRAMEBUFFER, multisampled.fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, resolved.fbo);
        glDisable(GL_SCISSOR_TEST);
        glBlitFramebuffer(0, 0, kWidth, kHeight, 0, 0, kWidth, kHeight, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        EXPECT_EQ(FirstGLError(), 0u) << "resolving a multisample depth buffer into a single-sampled one";

        glBindFramebuffer(GL_FRAMEBUFFER, resolved.fbo);
        const std::vector<float> depth = ReadDepthFloat(0, 0, kWidth, kHeight);
        EXPECT_EQ(FirstGLError(), 0u);
        ExpectAllDepth(depth, 0.875f, "resolved multisample depth");

        DestroySource(resolved);
        DestroySource(multisampled);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        Gl().EndFrame();
    }

    // A read whose rectangle is NOT the whole attachment. The staging copy has to carry
    // the requested rect (not the origin) and hand back its rows bottom-up, which a
    // full-extent uniform read is a fixed point of and therefore cannot see.
    TEST_F(DepthStencilReadbackMatrixScenario, ASubRectangleReadsTheRightBandInTheRightOrder) {
        if (!Ready()) return;
        DepthSource source = MakeTextureSource(GL_DEPTH24_STENCIL8);
        ASSERT_TRUE(SourceIsUsable());

        // Bottom half 0.25, top half 0.75, and the stencil banded the other way round so a
        // mix-up between the two aspects cannot pass either.
        glDisable(GL_SCISSOR_TEST);
        glViewport(0, 0, kWidth, kHeight);
        glDepthMask(GL_TRUE);
        glStencilMask(0xFFu);
        glEnable(GL_SCISSOR_TEST);
        glScissor(0, 0, kWidth, kHeight / 2);
        glClearDepth(0.25);
        glClearStencil(11);
        glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        glScissor(0, kHeight / 2, kWidth, kHeight - kHeight / 2);
        glClearDepth(0.75);
        glClearStencil(22);
        glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        glDisable(GL_SCISSOR_TEST);
        ASSERT_EQ(FirstGLError(), 0u);

        // A rect wholly inside the bottom band, offset from the origin in both axes.
        const int rectWidth = 8;
        const int rectHeight = 4;
        const std::vector<float> bottom = ReadDepthFloat(16, 4, rectWidth, rectHeight);
        EXPECT_EQ(FirstGLError(), 0u);
        ExpectAllDepth(bottom, 0.25f, "sub-rect inside the bottom depth band");
        const std::vector<int> bottomStencil = ReadStencilInt(16, 4, rectWidth, rectHeight);
        EXPECT_EQ(FirstGLError(), 0u);
        ExpectAllStencil(bottomStencil, 11, "sub-rect inside the bottom stencil band");

        // And one wholly inside the top band. Reading the mirrored row would answer 0.25.
        const std::vector<float> top = ReadDepthFloat(16, kHeight - 4 - rectHeight, rectWidth, rectHeight);
        EXPECT_EQ(FirstGLError(), 0u);
        ExpectAllDepth(top, 0.75f, "sub-rect inside the top depth band");

        // A rect that STRADDLES the boundary pins the row order itself: its first rows must
        // be the bottom band and its last rows the top one.
        const int straddleHeight = 8;
        const std::vector<float> straddle =
            ReadDepthFloat(16, kHeight / 2 - straddleHeight / 2, rectWidth, straddleHeight);
        EXPECT_EQ(FirstGLError(), 0u);
        ASSERT_EQ(straddle.size(), static_cast<size_t>(rectWidth) * straddleHeight);
        EXPECT_NEAR(straddle[0], 0.25f, 1.0f / 4096.0f)
            << "the first row of the returned rect must be its BOTTOM row (GL order), which is in the 0.25 band";
        EXPECT_NEAR(straddle[straddle.size() - 1], 0.75f, 1.0f / 4096.0f)
            << "the last row of the returned rect must be its TOP row, which is in the 0.75 band";

        DestroySource(source);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        Gl().EndFrame();
    }

    // The packed layouts the packed_depth_stencil family reads its gradients with.
    TEST_F(DepthStencilReadbackMatrixScenario, PackedDepthStencilReadPixelsCarriesBothAspects) {
        if (!Ready()) return;
        struct PackedCase {
            const char* name;
            GLenum internalFormat;
            GLenum type;
        };
        const PackedCase cases[] = {
            {"depth24_stencil8 / GL_UNSIGNED_INT_24_8", GL_DEPTH24_STENCIL8, GL_UNSIGNED_INT_24_8},
            {"depth32f_stencil8 / GL_FLOAT_32_UNSIGNED_INT_24_8_REV", GL_DEPTH32F_STENCIL8,
             GL_FLOAT_32_UNSIGNED_INT_24_8_REV},
        };
        int exercised = 0;
        for (const PackedCase& testCase : cases) {
            SCOPED_TRACE(testCase.name);
            DepthSource source = MakeTextureSource(testCase.internalFormat);
            if (!SourceIsUsable()) {
                DestroySource(source);
                continue;
            }
            FirstGLError();
            ClearDepthStencil(testCase.internalFormat, 0.5f, 3);
            ASSERT_EQ(FirstGLError(), 0u);

            const size_t pixels = static_cast<size_t>(kWidth) * kHeight;
            if (testCase.type == GL_UNSIGNED_INT_24_8) {
                std::vector<unsigned int> packed(pixels, kPacked24_8Poison);
                glReadPixels(0, 0, kWidth, kHeight, GL_DEPTH_STENCIL, testCase.type, packed.data());
                EXPECT_EQ(FirstGLError(), 0u);
                size_t bad = 0;
                for (unsigned int value : packed) {
                    const float depth = static_cast<float>(value >> 8) / 16777215.0f;
                    const int stencil = static_cast<int>(value & 0xFFu);
                    if (std::fabs(depth - 0.5f) > 0.01f || stencil != 3) ++bad;
                }
                EXPECT_EQ(bad, 0u) << testCase.name << ": " << bad << " of " << pixels
                                   << " packed words carry the wrong depth or stencil (first word 0x" << std::hex
                                   << packed[0] << std::dec << ")";
            } else {
                std::vector<D32fS8> packed(pixels, D32fS8{kDepthPoison, static_cast<unsigned int>(kStencilPoison)});
                glReadPixels(0, 0, kWidth, kHeight, GL_DEPTH_STENCIL, testCase.type, packed.data());
                EXPECT_EQ(FirstGLError(), 0u);
                size_t bad = 0;
                for (const D32fS8& value : packed) {
                    if (std::fabs(value.depth - 0.5f) > 0.01f || (value.stencil & 0xFFu) != 3u) ++bad;
                }
                EXPECT_EQ(bad, 0u) << testCase.name << ": " << bad << " of " << pixels
                                   << " packed pairs carry the wrong depth or stencil (first pair depth "
                                   << packed[0].depth << " stencil " << (packed[0].stencil & 0xFFu) << ")";
            }
            ++exercised;
            DestroySource(source);
        }
        EXPECT_GE(exercised, 1) << "neither packed depth/stencil format was renderable";
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        Gl().EndFrame();
    }

    // glGetTexImage reads a TEXTURE, not the bound framebuffer - a different entry point
    // that has to reach the same machinery. This is verify_get_tex_image's shape.
    TEST_F(DepthStencilReadbackMatrixScenario, GetTexImageReadsAPackedDepthStencilTexture) {
        if (!Ready()) return;
        DepthSource source = MakeTextureSource(GL_DEPTH24_STENCIL8);
        ASSERT_TRUE(SourceIsUsable());
        FirstGLError();
        ClearDepthStencil(GL_DEPTH24_STENCIL8, 0.375f, 5);
        ASSERT_EQ(FirstGLError(), 0u);

        // Read it back through the texture, with the framebuffer that owns it unbound so a
        // path that secretly read the framebuffer instead would answer from somewhere else.
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glBindTexture(GL_TEXTURE_2D, source.depthTexture);
        const size_t pixels = static_cast<size_t>(kWidth) * kHeight;
        std::vector<unsigned int> packed(pixels, kPacked24_8Poison);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, packed.data());
        EXPECT_EQ(FirstGLError(), 0u);
        size_t bad = 0;
        for (unsigned int value : packed) {
            const float depth = static_cast<float>(value >> 8) / 16777215.0f;
            if (std::fabs(depth - 0.375f) > 0.01f || (value & 0xFFu) != 5u) ++bad;
        }
        EXPECT_EQ(bad, 0u) << bad << " of " << pixels
                           << " words from glGetTexImage(GL_DEPTH_STENCIL) are wrong (first word 0x" << std::hex
                           << packed[0] << std::dec << ")";

        glBindTexture(GL_TEXTURE_2D, 0);
        DestroySource(source);
        Gl().EndFrame();
    }

    // glCopyTexImage2D out of a depth attachment, then read the copy - verify_copy_tex_image.
    TEST_F(DepthStencilReadbackMatrixScenario, CopyTexImageFromADepthAttachmentSurvivesAReadBack) {
        if (!Ready()) return;
        DepthSource source = MakeTextureSource(GL_DEPTH24_STENCIL8);
        ASSERT_TRUE(SourceIsUsable());
        FirstGLError();
        ClearDepthStencil(GL_DEPTH24_STENCIL8, 0.75f, 6);
        ASSERT_EQ(FirstGLError(), 0u);

        GLuint copy = 0;
        glGenTextures(1, &copy);
        glBindTexture(GL_TEXTURE_2D, copy);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, kWidth, kHeight, 0, GL_DEPTH_STENCIL,
                     GL_UNSIGNED_INT_24_8, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, 0, 0, kWidth, kHeight, 0);
        EXPECT_EQ(FirstGLError(), 0u) << "glCopyTexImage2D from a depth/stencil attachment";

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        const size_t pixels = static_cast<size_t>(kWidth) * kHeight;
        std::vector<unsigned int> packed(pixels, kPacked24_8Poison);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, packed.data());
        EXPECT_EQ(FirstGLError(), 0u);
        size_t bad = 0;
        for (unsigned int value : packed) {
            const float depth = static_cast<float>(value >> 8) / 16777215.0f;
            if (std::fabs(depth - 0.75f) > 0.01f) ++bad;
        }
        EXPECT_EQ(bad, 0u) << bad << " of " << pixels << " copied depth values are wrong (first word 0x" << std::hex
                           << packed[0] << std::dec << ")";

        glBindTexture(GL_TEXTURE_2D, 0);
        glDeleteTextures(1, &copy);
        DestroySource(source);
        Gl().EndFrame();
    }

    // The integer client widths, which are a separate conversion each.
    TEST_F(DepthStencilReadbackMatrixScenario, DepthAndStencilConvertIntoEveryClientWidth) {
        if (!Ready()) return;
        DepthSource source = MakeTextureSource(GL_DEPTH24_STENCIL8);
        ASSERT_TRUE(SourceIsUsable());
        FirstGLError();
        ClearDepthStencil(GL_DEPTH24_STENCIL8, 0.5f, 200);
        ASSERT_EQ(FirstGLError(), 0u);

        const size_t pixels = static_cast<size_t>(kWidth) * kHeight;

        std::vector<unsigned int> depthUint(pixels, 0xDEADBEEFu);
        glReadPixels(0, 0, kWidth, kHeight, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, depthUint.data());
        EXPECT_EQ(FirstGLError(), 0u) << "glReadPixels(GL_DEPTH_COMPONENT, GL_UNSIGNED_INT)";
        // 0.5 of the full 32-bit range, with room for the source's 24-bit quantisation.
        EXPECT_NEAR(static_cast<double>(depthUint[0]) / 4294967295.0, 0.5, 0.01)
            << "GL_UNSIGNED_INT depth came back as " << depthUint[0];

        std::vector<unsigned short> depthUshort(pixels, 0xBEEFu);
        glReadPixels(0, 0, kWidth, kHeight, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT, depthUshort.data());
        EXPECT_EQ(FirstGLError(), 0u) << "glReadPixels(GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT)";
        EXPECT_NEAR(static_cast<double>(depthUshort[0]) / 65535.0, 0.5, 0.01)
            << "GL_UNSIGNED_SHORT depth came back as " << depthUshort[0];

        // A stencil index is written unconverted into whichever width was asked for, so 200
        // must survive intact in all of them - it is also large enough that a signed byte
        // would wrap, which is the point of choosing it.
        std::vector<unsigned char> stencilByte(pixels, static_cast<unsigned char>(kStencilPoison));
        glReadPixels(0, 0, kWidth, kHeight, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, stencilByte.data());
        EXPECT_EQ(FirstGLError(), 0u) << "glReadPixels(GL_STENCIL_INDEX, GL_UNSIGNED_BYTE)";
        EXPECT_EQ(static_cast<int>(stencilByte[0]), 200);

        std::vector<int> stencilInt(pixels, kStencilPoison);
        glReadPixels(0, 0, kWidth, kHeight, GL_STENCIL_INDEX, GL_INT, stencilInt.data());
        EXPECT_EQ(FirstGLError(), 0u) << "glReadPixels(GL_STENCIL_INDEX, GL_INT)";
        EXPECT_EQ(stencilInt[0], 200);

        DestroySource(source);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        Gl().EndFrame();
    }

    // The PACK pixel-store parameters apply to a depth read exactly as they do to a colour
    // one, and the gap regions they create must be left alone.
    TEST_F(DepthStencilReadbackMatrixScenario, DepthReadbackHonoursThePackPixelStoreParameters) {
        if (!Ready()) return;
        DepthSource source = MakeTextureSource(GL_DEPTH_COMPONENT24);
        ASSERT_TRUE(SourceIsUsable());
        FirstGLError();
        ClearDepthStencil(GL_DEPTH_COMPONENT24, 0.5f, 0);
        ASSERT_EQ(FirstGLError(), 0u);

        const int rectWidth = 4;
        const int rectHeight = 3;
        const int rowLength = 8;
        const int skipPixels = 2;
        const int skipRows = 1;
        constexpr float kGap = -7.0f;
        std::vector<float> destination(static_cast<size_t>(rowLength) * (skipRows + rectHeight) + 16, kGap);

        glPixelStorei(GL_PACK_ROW_LENGTH, rowLength);
        glPixelStorei(GL_PACK_SKIP_PIXELS, skipPixels);
        glPixelStorei(GL_PACK_SKIP_ROWS, skipRows);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glReadPixels(0, 0, rectWidth, rectHeight, GL_DEPTH_COMPONENT, GL_FLOAT, destination.data());
        const unsigned int readError = FirstGLError();
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
        glPixelStorei(GL_PACK_SKIP_ROWS, 0);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        EXPECT_EQ(readError, 0u);

        size_t written = 0;
        size_t gapsTouched = 0;
        for (size_t index = 0; index < destination.size(); ++index) {
            const long row = static_cast<long>(index) / rowLength - skipRows;
            const long column = static_cast<long>(index) % rowLength - skipPixels;
            const bool inRect = row >= 0 && row < rectHeight && column >= 0 && column < rectWidth;
            if (inRect) {
                if (std::fabs(destination[index] - 0.5f) <= 1.0f / 4096.0f) ++written;
            } else if (destination[index] != kGap) {
                ++gapsTouched;
            }
        }
        EXPECT_EQ(written, static_cast<size_t>(rectWidth) * rectHeight)
            << "only " << written << " of " << (rectWidth * rectHeight)
            << " destination pixels landed where GL_PACK_ROW_LENGTH/SKIP_* put them";
        EXPECT_EQ(gapsTouched, 0u) << gapsTouched << " bytes outside the packed rectangle were overwritten";

        DestroySource(source);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        Gl().EndFrame();
    }

    // The readback borrows the application's context for a full-screen pass. Everything it
    // touches has to come back, or the next draw inherits it - which is how an emulation
    // that "works" takes the rest of the renderer down with it.
    TEST_F(DepthStencilReadbackMatrixScenario, ReadbackLeavesNoGLStateBehind) {
        if (!Ready()) return;
        DepthSource source = MakeTextureSource(GL_DEPTH24_STENCIL8);
        ASSERT_TRUE(SourceIsUsable());
        FirstGLError();
        ClearDepthStencil(GL_DEPTH24_STENCIL8, 0.5f, 4);

        // A deliberately awkward state: nothing here is what an emulation pass would want,
        // so anything it forgets to put back shows up below.
        GLuint scratchTexture = 0;
        glGenTextures(1, &scratchTexture);
        glBindTexture(GL_TEXTURE_2D, scratchTexture);
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, scratchTexture);
        glEnable(GL_SCISSOR_TEST);
        glScissor(3, 5, 7, 11);
        glEnable(GL_CULL_FACE);
        glEnable(GL_BLEND);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_GEQUAL);
        glDepthMask(GL_FALSE);
        glEnable(GL_STENCIL_TEST);
        glStencilFunc(GL_NOTEQUAL, 0x5, 0x0Fu);
        glStencilOp(GL_INCR, GL_DECR, GL_INVERT);
        glStencilMask(0x3Cu);
        glColorMask(GL_FALSE, GL_TRUE, GL_FALSE, GL_TRUE);
        glViewport(2, 3, 5, 7);
        ASSERT_EQ(FirstGLError(), 0u);

        const std::vector<float> depth = ReadDepthFloat(0, 0, kWidth, kHeight);
        const std::vector<int> stencil = ReadStencilInt(0, 0, kWidth, kHeight);
        EXPECT_EQ(FirstGLError(), 0u);
        ExpectAllDepth(depth, 0.5f, "state-preservation case depth");
        ExpectAllStencil(stencil, 4, "state-preservation case stencil");

        GLint viewport[4] = {0, 0, 0, 0};
        GLint scissorBox[4] = {0, 0, 0, 0};
        GLboolean colorMask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
        GLint depthFunc = 0;
        GLboolean depthMask = GL_TRUE;
        GLint stencilFunc = 0, stencilRef = 0, stencilValueMask = 0, stencilWriteMask = 0;
        GLint stencilFail = 0, stencilPassDepthFail = 0, stencilPassDepthPass = 0;
        GLint activeTexture = 0, boundTexture = 0;
        glGetIntegerv(GL_VIEWPORT, viewport);
        glGetIntegerv(GL_SCISSOR_BOX, scissorBox);
        glGetBooleanv(GL_COLOR_WRITEMASK, colorMask);
        glGetIntegerv(GL_DEPTH_FUNC, &depthFunc);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
        glGetIntegerv(GL_STENCIL_FUNC, &stencilFunc);
        glGetIntegerv(GL_STENCIL_REF, &stencilRef);
        glGetIntegerv(GL_STENCIL_VALUE_MASK, &stencilValueMask);
        glGetIntegerv(GL_STENCIL_WRITEMASK, &stencilWriteMask);
        glGetIntegerv(GL_STENCIL_FAIL, &stencilFail);
        glGetIntegerv(GL_STENCIL_PASS_DEPTH_FAIL, &stencilPassDepthFail);
        glGetIntegerv(GL_STENCIL_PASS_DEPTH_PASS, &stencilPassDepthPass);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &boundTexture);

        EXPECT_EQ(viewport[0], 2);
        EXPECT_EQ(viewport[1], 3);
        EXPECT_EQ(viewport[2], 5);
        EXPECT_EQ(viewport[3], 7);
        EXPECT_EQ(scissorBox[0], 3);
        EXPECT_EQ(scissorBox[1], 5);
        EXPECT_EQ(scissorBox[2], 7);
        EXPECT_EQ(scissorBox[3], 11);
        EXPECT_EQ(glIsEnabled(GL_SCISSOR_TEST), GLboolean(GL_TRUE));
        EXPECT_EQ(glIsEnabled(GL_CULL_FACE), GLboolean(GL_TRUE));
        EXPECT_EQ(glIsEnabled(GL_BLEND), GLboolean(GL_TRUE));
        EXPECT_EQ(glIsEnabled(GL_DEPTH_TEST), GLboolean(GL_TRUE));
        EXPECT_EQ(glIsEnabled(GL_STENCIL_TEST), GLboolean(GL_TRUE));
        EXPECT_EQ(colorMask[0], GLboolean(GL_FALSE));
        EXPECT_EQ(colorMask[1], GLboolean(GL_TRUE));
        EXPECT_EQ(colorMask[2], GLboolean(GL_FALSE));
        EXPECT_EQ(colorMask[3], GLboolean(GL_TRUE));
        EXPECT_EQ(depthFunc, GLint(GL_GEQUAL));
        EXPECT_EQ(depthMask, GLboolean(GL_FALSE));
        EXPECT_EQ(stencilFunc, GLint(GL_NOTEQUAL));
        EXPECT_EQ(stencilRef, 0x5);
        EXPECT_EQ(stencilValueMask, 0x0F);
        EXPECT_EQ(stencilWriteMask, 0x3C);
        EXPECT_EQ(stencilFail, GLint(GL_INCR));
        EXPECT_EQ(stencilPassDepthFail, GLint(GL_DECR));
        EXPECT_EQ(stencilPassDepthPass, GLint(GL_INVERT));
        EXPECT_EQ(activeTexture, GLint(GL_TEXTURE3));
        EXPECT_EQ(boundTexture, GLint(scratchTexture))
            << "the readback left a scratch texture on the application's texture unit";
        EXPECT_EQ(FirstGLError(), 0u);

        // Put the awkward state back so the next scenario in this process starts clean.
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_BLEND);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_STENCIL_TEST);
        glDepthFunc(GL_LESS);
        glDepthMask(GL_TRUE);
        glStencilFunc(GL_ALWAYS, 0, 0xFFFFFFFFu);
        glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
        glStencilMask(0xFFFFFFFFu);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0);
        glDeleteTextures(1, &scratchTexture);
        DestroySource(source);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, Gl().Width(), Gl().Height());
        Gl().EndFrame();
    }

} // namespace MGITest
