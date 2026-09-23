// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/BandedReadbackScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// P7 gate 5, package g5-readback: readbacks LARGER THAN ONE REPLY SLOT.
//
// On every split arm a readback's answer travels back in one SEG_REPLY slot, and a slot carries
// at most MaxReplyBytes = 2 MiB - 16 = 2,097,136 bytes (ID-47). Until this package a
// glReadPixels whose tight answer was larger than that was Fatal{ReplyTooLarge} at the client -
// KHR-GL46.direct_state_access.renderbuffers_storage reads 256x512 RGBA/FLOAT = 2,097,152 bytes,
// sixteen over, and aborted on the inproc arm while the monolith passed. The client now splits
// such a read into row bands that each fit a slot (EmitTables.h PlanReadbackBands) and scatters
// every band under the application's GL_PACK_* state.
//
// THESE ARE ORDINARY GL CASES and run on the monolith arms too, which is the parity half: the
// same pixels must come back whichever side of the wire answers. Every shape below straddles
// the slot boundary on purpose - exactly 2 MiB, 2 MiB plus one row, more than the WHOLE 16 MiB
// SEG_REPLY - and every image is POSITION-DEPENDENT, so a band that landed at the wrong row,
// was dropped, or was read from the wrong box cannot look right.
//
// A row wider than a whole reply (the column-piece arm of the plan) is not reachable here: it
// needs a >131071-pixel RGBA/FLOAT row, wider than any attachment, and GL leaves the pixels
// outside the framebuffer undefined - so that arm is covered by the unit controls over the
// plan and the band scatter (MG_Test/Wire/RemoteClientTest.cpp, RemoteReadback.*).

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
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

        // The shared-memory links' reply payload cap (ID-47). Named here only to state the
        // arithmetic each shape straddles; the client reads the live number from its link.
        constexpr std::size_t kReplySlotPayload = 2u * 1024u * 1024u - 16u;
        // The whole SEG_REPLY segment: eight slots of 2 MiB.
        constexpr std::size_t kReplySegmentBytes = 16u * 1024u * 1024u;

        // A texel that names its own position, so a misplaced band cannot match.
        std::array<GLubyte, 4> PatternRgba8(int x, int y) {
            return {static_cast<GLubyte>(x & 0xFF),
                    static_cast<GLubyte>(((x >> 8) & 0x0F) | (((y >> 8) & 0x0F) << 4)),
                    static_cast<GLubyte>(y & 0xFF), static_cast<GLubyte>((x * 3 + y * 5) & 0xFF)};
        }

        // Every component an integer (or a half) below 2^24, so float storage is exact.
        std::array<GLfloat, 4> PatternRgba32f(int x, int y) {
            return {static_cast<GLfloat>(x), static_cast<GLfloat>(y),
                    static_cast<GLfloat>(x) * 0.5f + static_cast<GLfloat>(y) * 1024.0f,
                    static_cast<GLfloat>(1 + (x ^ y))};
        }

        struct PatternFormat {
            const char* name;
            GLenum internalFormat;
            GLenum type;
            std::size_t bytesPerPixel;
        };
        constexpr PatternFormat kRgba8{"RGBA8 / UNSIGNED_BYTE", GL_RGBA8, GL_UNSIGNED_BYTE, 4};
        constexpr PatternFormat kRgba32f{"RGBA32F / FLOAT", GL_RGBA32F, GL_FLOAT, 16};

        // The pattern's bytes for one texel, in the format's client layout.
        void PatternBytes(const PatternFormat& format, int x, int y, GLubyte* out) {
            if (format.type == GL_FLOAT) {
                const auto texel = PatternRgba32f(x, y);
                std::memcpy(out, texel.data(), sizeof(texel));
            } else {
                const auto texel = PatternRgba8(x, y);
                std::memcpy(out, texel.data(), sizeof(texel));
            }
        }

        class BandedReadbackScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                DrainErrors();
                glGenFramebuffers(1, &m_fbo);
                glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
                glDisable(GL_SCISSOR_TEST);
                glDisable(GL_DEPTH_TEST);
                glDisable(GL_STENCIL_TEST);
                glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                glDepthMask(GL_TRUE);
                glStencilMask(~0u);
                ResetPack();
            }

            void TearDown() override {
                if (!Ready()) return;
                ResetPack();
                glDisable(GL_SCISSOR_TEST);
                glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
                if (m_pbo != 0) glDeleteBuffers(1, &m_pbo);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                if (m_fbo != 0) glDeleteFramebuffers(1, &m_fbo);
                for (GLuint texture : m_textures) glDeleteTextures(1, &texture);
                for (GLuint renderbuffer : m_renderbuffers) glDeleteRenderbuffers(1, &renderbuffer);
                m_pbo = 0;
                m_fbo = 0;
                m_textures.clear();
                m_renderbuffers.clear();
                DrainErrors();
                ScenarioTest::TearDown();
            }

            static void ResetPack() {
                glPixelStorei(GL_PACK_ALIGNMENT, 4);
                glPixelStorei(GL_PACK_ROW_LENGTH, 0);
                glPixelStorei(GL_PACK_SKIP_ROWS, 0);
                glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            }

            static void DrainErrors() {
                for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; ++i) {
                }
            }

            // A texture holding the position pattern, attached as COLOR_ATTACHMENT0 and read.
            void AttachPatternedTexture(const PatternFormat& format, int width, int height) {
                std::vector<GLubyte> texels(static_cast<std::size_t>(width) * height * format.bytesPerPixel);
                for (int y = 0; y < height; ++y) {
                    for (int x = 0; x < width; ++x) {
                        PatternBytes(format, x, y,
                                     texels.data() + (static_cast<std::size_t>(y) * width + x) * format.bytesPerPixel);
                    }
                }
                GLuint texture = 0;
                glGenTextures(1, &texture);
                m_textures.push_back(texture);
                glBindTexture(GL_TEXTURE_2D, texture);
                glTexStorage2D(GL_TEXTURE_2D, 1, format.internalFormat, width, height);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, format.type, texels.data());
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
                glReadBuffer(GL_COLOR_ATTACHMENT0);
                ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE))
                    << format.name << " " << width << "x" << height;
                ASSERT_EQ(FirstGLError(), static_cast<GLenum>(GL_NO_ERROR)) << "pattern upload " << format.name;
            }

            // Every texel of a tight w*h read at (x0, y0), against the pattern.
            static void ExpectTightPattern(const PatternFormat& format, const std::vector<GLubyte>& actual, int x0,
                                           int y0, int width, int height) {
                std::array<GLubyte, 16> expected{};
                std::size_t bad = 0;
                std::string first;
                for (int y = 0; y < height; ++y) {
                    for (int x = 0; x < width; ++x) {
                        PatternBytes(format, x0 + x, y0 + y, expected.data());
                        const GLubyte* got =
                            actual.data() + (static_cast<std::size_t>(y) * width + x) * format.bytesPerPixel;
                        if (std::memcmp(got, expected.data(), format.bytesPerPixel) != 0) {
                            if (bad++ == 0) first = "(" + std::to_string(x) + "," + std::to_string(y) + ")";
                        }
                    }
                }
                EXPECT_EQ(bad, 0u) << format.name << " " << width << "x" << height << ": " << bad
                                   << " texels differ from the pattern, first at " << first;
            }

            void ReadWholeAndExpect(const PatternFormat& format, int width, int height) {
                AttachPatternedTexture(format, width, height);
                if (HasFatalFailure()) return;
                std::vector<GLubyte> actual(static_cast<std::size_t>(width) * height * format.bytesPerPixel, 0xC3);
                glReadPixels(0, 0, width, height, GL_RGBA, format.type, actual.data());
                ASSERT_EQ(FirstGLError(), static_cast<GLenum>(GL_NO_ERROR)) << "glReadPixels " << format.name;
                ExpectTightPattern(format, actual, 0, 0, width, height);
            }

            GLuint m_fbo = 0;
            GLuint m_pbo = 0;
            std::vector<GLuint> m_textures;
            std::vector<GLuint> m_renderbuffers;
        };

    } // namespace

    // THE CTS SHAPE, byte for byte: KHR-GL46.direct_state_access.renderbuffers_storage reads a
    // 256x512 RGBA32F renderbuffer as GL_RGBA/GL_FLOAT - 2,097,152 bytes, sixteen more than a slot.
    // A renderbuffer cannot be uploaded, so it is painted with scissored clears: 16 row stripes x 2
    // column halves, each its own colour (every value a multiple of 1/32, exact in float and inside
    // [0, 1] so no backend's clamping can move it). The last band is the single top row.
    TEST_F(BandedReadbackScenario, RenderbufferReadOfExactlyTwoMiBIsBandedNotRefused) {
        if (!Ready()) return;
        constexpr int kWidth = 256;
        constexpr int kHeight = 512;
        static_assert(static_cast<std::size_t>(kWidth) * kHeight * 16 == kReplySlotPayload + 16,
                      "the CTS read is exactly 2 MiB, sixteen bytes over one reply slot");
        GLuint renderbuffer = 0;
        glGenRenderbuffers(1, &renderbuffer);
        m_renderbuffers.push_back(renderbuffer);
        glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA32F, kWidth, kHeight);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, renderbuffer);
        glDrawBuffer(GL_COLOR_ATTACHMENT0);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));

        auto stripeColour = [](int x, int y) {
            const int stripe = y / 32;
            const int half = x / 128;
            return std::array<GLfloat, 4>{static_cast<GLfloat>(stripe) / 16.0f, static_cast<GLfloat>(half),
                                          static_cast<GLfloat>(stripe * 2 + half) / 32.0f,
                                          0.25f + 0.5f * static_cast<GLfloat>(half)};
        };
        glEnable(GL_SCISSOR_TEST);
        for (int stripe = 0; stripe < kHeight / 32; ++stripe) {
            for (int half = 0; half < 2; ++half) {
                const auto colour = stripeColour(half * 128, stripe * 32);
                glScissor(half * 128, stripe * 32, 128, 32);
                glClearColor(colour[0], colour[1], colour[2], colour[3]);
                glClear(GL_COLOR_BUFFER_BIT);
            }
        }
        glDisable(GL_SCISSOR_TEST);
        ASSERT_EQ(FirstGLError(), static_cast<GLenum>(GL_NO_ERROR)) << "stripe clears";

        std::vector<GLfloat> pixels(static_cast<std::size_t>(kWidth) * kHeight * 4, -7.0f);
        glReadPixels(0, 0, kWidth, kHeight, GL_RGBA, GL_FLOAT, pixels.data());
        ASSERT_EQ(FirstGLError(), static_cast<GLenum>(GL_NO_ERROR)) << "glReadPixels RGBA/FLOAT 256x512";
        std::size_t bad = 0;
        std::string first;
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                const auto expected = stripeColour(x, y);
                const GLfloat* got = pixels.data() + (static_cast<std::size_t>(y) * kWidth + x) * 4;
                if (std::memcmp(got, expected.data(), sizeof(expected)) != 0 && bad++ == 0) {
                    first = "(" + std::to_string(x) + "," + std::to_string(y) + ") = " + std::to_string(got[0]) +
                            "," + std::to_string(got[1]) + "," + std::to_string(got[2]) + "," +
                            std::to_string(got[3]);
                }
            }
        }
        EXPECT_EQ(bad, 0u) << bad << " of " << kWidth * kHeight << " texels are wrong, first " << first;
    }

    // One row over the slot: 1024x513 RGBA8 = 2 MiB + 4 KiB. Two bands, the second a single row.
    TEST_F(BandedReadbackScenario, TwoMiBPlusOneRowIsBanded) {
        if (!Ready()) return;
        static_assert(1024u * 512u * 4u > kReplySlotPayload, "512 rows alone are already over a slot");
        ReadWholeAndExpect(kRgba8, 1024, 513);
    }

    // Larger than the WHOLE reply segment, not just one slot: 2048x2100 RGBA8 = 17,203,200 bytes
    // against a 16 MiB SEG_REPLY. A reply pool that merely grew could never answer this; nine bands
    // through the same slot ring can.
    TEST_F(BandedReadbackScenario, AReadLargerThanTheWholeReplySegmentIsBanded) {
        if (!Ready()) return;
        static_assert(2048u * 2100u * 4u > kReplySegmentBytes, "the read must outgrow SEG_REPLY itself");
        ReadWholeAndExpect(kRgba8, 2048, 2100);
    }

    // The bounce path: a non-neutral pack state, for both a 4- and a 16-byte pixel, read from an
    // offset box so every band's own box origin is exercised as well. PACK_ALIGNMENT 8 with an odd
    // ROW_LENGTH pads every row, SKIP_ROWS / SKIP_PIXELS move the first byte - and the bytes GL does
    // NOT name must keep their sentinel, which is what a band scattered on the wrong stride breaks.
    TEST_F(BandedReadbackScenario, BandedReadsHonourPackAlignmentRowLengthAndSkips) {
        if (!Ready()) return;
        struct PackCase {
            PatternFormat format;
            int textureWidth, textureHeight;
            int x0, y0, width, height;
            int alignment, rowLength, skipRows, skipPixels;
        };
        const PackCase cases[] = {
            {kRgba8, 1100, 700, 3, 5, 1023, 600, 8, 1025, 2, 5},
            {kRgba32f, 320, 640, 1, 2, 300, 600, 8, 303, 1, 2},
        };
        for (const PackCase& c : cases) {
            SCOPED_TRACE(c.format.name);
            ASSERT_GT(static_cast<std::size_t>(c.width) * c.height * c.format.bytesPerPixel, kReplySlotPayload);
            AttachPatternedTexture(c.format, c.textureWidth, c.textureHeight);
            if (HasFatalFailure()) return;
            const std::size_t bpp = c.format.bytesPerPixel;
            const std::size_t stride =
                (static_cast<std::size_t>(c.rowLength) * bpp + c.alignment - 1) / c.alignment * c.alignment;
            const std::size_t first = static_cast<std::size_t>(c.skipRows) * stride + c.skipPixels * bpp;
            const std::size_t size = first + static_cast<std::size_t>(c.height) * stride + 64;
            constexpr GLubyte kSentinel = 0xA5;
            std::vector<GLubyte> destination(size, kSentinel);
            glPixelStorei(GL_PACK_ALIGNMENT, c.alignment);
            glPixelStorei(GL_PACK_ROW_LENGTH, c.rowLength);
            glPixelStorei(GL_PACK_SKIP_ROWS, c.skipRows);
            glPixelStorei(GL_PACK_SKIP_PIXELS, c.skipPixels);
            glReadPixels(c.x0, c.y0, c.width, c.height, GL_RGBA, c.format.type, destination.data());
            ResetPack();
            ASSERT_EQ(FirstGLError(), static_cast<GLenum>(GL_NO_ERROR));

            std::vector<GLubyte> wanted(size, kSentinel);
            for (int y = 0; y < c.height; ++y) {
                for (int x = 0; x < c.width; ++x) {
                    PatternBytes(c.format, c.x0 + x, c.y0 + y, wanted.data() + first + y * stride + x * bpp);
                }
            }
            std::size_t bad = 0, firstBad = 0;
            for (std::size_t i = 0; i < size; ++i) {
                if (destination[i] != wanted[i] && bad++ == 0) firstBad = i;
            }
            EXPECT_EQ(bad, 0u) << bad << " of " << size << " destination bytes differ, first at byte " << firstBad
                               << " (row " << (firstBad >= first ? (firstBad - first) / stride : 0) << ")";
        }
    }

    // A PIXEL_PACK_BUFFER destination at a non-zero offset: the bands are uploaded row by row into
    // the buffer under the pack state, and the bytes around them are the application's.
    TEST_F(BandedReadbackScenario, BandedReadIntoAPixelPackBufferKeepsItsPadding) {
        if (!Ready()) return;
        constexpr int kWidth = 1024;
        constexpr int kHeight = 600;
        constexpr int kRowLength = 1030;
        constexpr int kSkipRows = 1;
        constexpr std::size_t kOffset = 16;
        static_assert(static_cast<std::size_t>(kWidth) * kHeight * 4 > kReplySlotPayload, "must band");
        AttachPatternedTexture(kRgba8, kWidth, kHeight);
        if (HasFatalFailure()) return;
        const std::size_t stride = static_cast<std::size_t>(kRowLength) * 4;
        const std::size_t first = kOffset + kSkipRows * stride;
        const std::size_t size = first + static_cast<std::size_t>(kHeight) * stride + 32;
        constexpr GLubyte kSentinel = 0x3C;
        const std::vector<GLubyte> initial(size, kSentinel);
        glGenBuffers(1, &m_pbo);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo);
        glBufferData(GL_PIXEL_PACK_BUFFER, static_cast<GLsizeiptr>(size), initial.data(), GL_DYNAMIC_READ);
        glPixelStorei(GL_PACK_ROW_LENGTH, kRowLength);
        glPixelStorei(GL_PACK_SKIP_ROWS, kSkipRows);
        glReadPixels(0, 0, kWidth, kHeight, GL_RGBA, GL_UNSIGNED_BYTE, reinterpret_cast<void*>(kOffset));
        ResetPack();
        ASSERT_EQ(FirstGLError(), static_cast<GLenum>(GL_NO_ERROR));
        std::vector<GLubyte> actual(size, 0);
        glGetBufferSubData(GL_PIXEL_PACK_BUFFER, 0, static_cast<GLsizeiptr>(size), actual.data());
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        ASSERT_EQ(FirstGLError(), static_cast<GLenum>(GL_NO_ERROR));

        std::vector<GLubyte> wanted = initial;
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                const auto texel = PatternRgba8(x, y);
                std::memcpy(wanted.data() + first + y * stride + x * 4, texel.data(), 4);
            }
        }
        std::size_t bad = 0, firstBad = 0;
        for (std::size_t i = 0; i < size; ++i) {
            if (actual[i] != wanted[i] && bad++ == 0) firstBad = i;
        }
        EXPECT_EQ(bad, 0u) << bad << " of " << size << " buffer bytes differ, first at byte " << firstBad;
    }

    // Depth and packed depth/stencil take the SAME banded path - the plan is about the layout,
    // not the component. DEPTH_COMPONENT/FLOAT is 4 bytes a pixel (2 bands here), and
    // DEPTH_STENCIL/FLOAT_32_UNSIGNED_INT_24_8_REV is 8 (3 bands). Depth is painted in row
    // stripes and stencil in column quarters, so both axes of a band's placement are checked.
    TEST_F(BandedReadbackScenario, BandedDepthAndDepthStencilReads) {
        if (!Ready()) return;
        constexpr int kWidth = 1024;
        constexpr int kHeight = 640;
        constexpr int kStripeRows = 40;
        static_assert(static_cast<std::size_t>(kWidth) * kHeight * 4 > kReplySlotPayload, "depth must band");
        GLuint texture = 0;
        glGenTextures(1, &texture);
        m_textures.push_back(texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_DEPTH32F_STENCIL8, kWidth, kHeight);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, texture, 0);
        glDrawBuffer(GL_NONE);
        glReadBuffer(GL_NONE);
        ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));

        auto depthAt = [](int y) { return static_cast<GLfloat>(y / kStripeRows + 1) / 32.0f; };
        auto stencilAt = [](int x) { return static_cast<unsigned>(10 + (x / (kWidth / 4)) * 20); };
        glEnable(GL_SCISSOR_TEST);
        for (int stripe = 0; stripe < kHeight / kStripeRows; ++stripe) {
            glScissor(0, stripe * kStripeRows, kWidth, kStripeRows);
            glClearDepth(depthAt(stripe * kStripeRows));
            glClear(GL_DEPTH_BUFFER_BIT);
        }
        for (int quarter = 0; quarter < 4; ++quarter) {
            glScissor(quarter * (kWidth / 4), 0, kWidth / 4, kHeight);
            glClearStencil(static_cast<GLint>(stencilAt(quarter * (kWidth / 4))));
            glClear(GL_STENCIL_BUFFER_BIT);
        }
        glDisable(GL_SCISSOR_TEST);
        glClearDepth(1.0);
        glClearStencil(0);
        ASSERT_EQ(FirstGLError(), static_cast<GLenum>(GL_NO_ERROR)) << "depth/stencil stripe clears";

        const std::size_t pixels = static_cast<std::size_t>(kWidth) * kHeight;
        {
            std::vector<GLfloat> depth(pixels, -3.0f);
            glReadPixels(0, 0, kWidth, kHeight, GL_DEPTH_COMPONENT, GL_FLOAT, depth.data());
            ASSERT_EQ(FirstGLError(), static_cast<GLenum>(GL_NO_ERROR)) << "DEPTH_COMPONENT/FLOAT";
            std::size_t bad = 0, firstBad = 0;
            for (std::size_t i = 0; i < pixels; ++i) {
                const int y = static_cast<int>(i / kWidth);
                if (std::fabs(depth[i] - depthAt(y)) > 1.0f / 4096.0f && bad++ == 0) firstBad = i;
            }
            EXPECT_EQ(bad, 0u) << bad << " depth values wrong, first at pixel " << firstBad << " = "
                               << depth[firstBad];
        }
        {
            struct D32fS8 {
                GLfloat depth;
                GLuint stencil;
            };
            static_assert(sizeof(D32fS8) == 8, "FLOAT_32_UNSIGNED_INT_24_8_REV is two 32-bit words");
            std::vector<D32fS8> packed(pixels, D32fS8{-3.0f, 0xFFFFFFFFu});
            glReadPixels(0, 0, kWidth, kHeight, GL_DEPTH_STENCIL, GL_FLOAT_32_UNSIGNED_INT_24_8_REV, packed.data());
            ASSERT_EQ(FirstGLError(), static_cast<GLenum>(GL_NO_ERROR)) << "DEPTH_STENCIL/FLOAT_32_24_8_REV";
            std::size_t bad = 0, firstBad = 0;
            for (std::size_t i = 0; i < pixels; ++i) {
                const int x = static_cast<int>(i % kWidth);
                const int y = static_cast<int>(i / kWidth);
                if ((std::fabs(packed[i].depth - depthAt(y)) > 1.0f / 4096.0f ||
                     (packed[i].stencil & 0xFFu) != stencilAt(x)) &&
                    bad++ == 0) {
                    firstBad = i;
                }
            }
            EXPECT_EQ(bad, 0u) << bad << " packed depth/stencil pairs wrong, first at pixel " << firstBad
                               << " = depth " << packed[firstBad].depth << " stencil "
                               << (packed[firstBad].stencil & 0xFFu);
        }
    }

    // THE OTHER READBACK THAT CROSSES THE REPLY SLOT. glGetTexImage was already windowed
    // (TextureReadbackEmit.inc: DstOffset/DstSize windows of at most MaxReplyBytes), so this is
    // the parity row that keeps it so beside the banded ReadPixels: 1024x1024 RGBA8 = 4 MiB.
    TEST_F(BandedReadbackScenario, GetTexImageLargerThanAReplySlotIsWindowed) {
        if (!Ready()) return;
        constexpr int kWidth = 1024;
        constexpr int kHeight = 1024;
        AttachPatternedTexture(kRgba8, kWidth, kHeight);
        if (HasFatalFailure()) return;
        std::vector<GLubyte> actual(static_cast<std::size_t>(kWidth) * kHeight * 4, 0x77);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, actual.data());
        ASSERT_EQ(FirstGLError(), static_cast<GLenum>(GL_NO_ERROR)) << "glGetTexImage";
        ExpectTightPattern(kRgba8, actual, 0, 0, kWidth, kHeight);
    }

} // namespace MGITest
