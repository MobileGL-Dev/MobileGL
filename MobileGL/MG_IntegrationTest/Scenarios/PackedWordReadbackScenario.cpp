// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/PackedWordReadbackScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// glGetTexImage of a 32-bit packed format read with its OWN client type owes the application the
// words the image HOLDS, and KHR-GL43.copy_image compares exactly those words. Two routes used to
// answer, and both are wrong for a level glCopyImageSubData wrote:
//
//   * the colour-attachment route reads GL_RGBA/GL_FLOAT and re-encodes, which canonicalizes an
//     RGB9_E5 shared exponent and collapses an R11F_G11F_B10F NaN payload to 1;
//   * the CPU shadow only holds what was UPLOADED, and the mirror that replays a copy into it
//     declines - silently - for a renderbuffer source, which has no shadow to mirror from.
//
// Both are pinned here with words the CTS itself uses, because both failures are invisible to a
// value comparison: every assertion below is on BITS that decode to the very value the wrong
// answer also decodes to.
//
// The fix is a raw-word route (DirectGLES::ReadPackedLevelWordsViaScratch: copy the level into a
// scratch GL_R32UI image, read that back as unsigned integers), and DirectVulkan reaches the same
// place through PackReadbackToClientOrPbo's raw-word branch over the staging bytes - so these
// scenarios are backend-agnostic on purpose.

#include <cstddef>
#include <ios>
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

        constexpr GLsizei kExtent = 4;

        // The non-canonical RGB9_E5 word KHR-GL43.copy_image writes: R=0, G=0, B mantissa 63,
        // shared exponent 31, i.e. the value 8064, which the spec's own encoder would emit as
        // 0xe7e00000 instead. Anything that decodes and re-encodes hands back the canonical word.
        //
        // Reinterpreted in the destination of an RGB9_E5 -> R11F_G11F_B10F copy it is R=0,
        // G=1920, B=995 - and B's 5-bit exponent is all ones with a nonzero mantissa, i.e. a NaN
        // whose payload 3 does not survive a float32 round trip (it comes back as the canonical
        // payload 1, B=993, word 0xf87c0000). The two defects therefore land on the same word.
        constexpr GLuint kRgb9E5Word = 0xf8fc0000u;

        // The R11F_G11F_B10F word the same test pairs with it: R=0, G=0, B = exponent 12,
        // mantissa 0 = 0.125. As an RGB9_E5 word it is all-zero channels with a shared exponent of
        // 12, which the canonical encoder would write as 0x00000000 - so a decode/re-encode of THIS
        // one loses every bit that distinguishes it.
        constexpr GLuint kR11fG11fB10fWord = 0x60000000u;

        class PackedWordReadbackScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                DrainErrors();
            }

            void TearDown() override {
                if (!Ready()) return;
                DeleteObjects();
                DrainErrors();
                ScenarioTest::TearDown();
            }

            static void DrainErrors() {
                for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; ++i) {
                }
            }

            void DeleteObjects() {
                if (m_src != 0) glDeleteTextures(1, &m_src);
                if (m_dst != 0) glDeleteTextures(1, &m_dst);
                if (m_rbo != 0) glDeleteRenderbuffers(1, &m_rbo);
                m_src = 0;
                m_dst = 0;
                m_rbo = 0;
            }

            // A complete single-level texture whose every texel holds `word`, uploaded through the
            // packed client type so the stored bits are the client's bits and nothing has had a
            // chance to re-encode them.
            GLuint MakePackedTexture(GLenum internalFormat, GLenum type, GLuint word) {
                const std::vector<GLuint> words(static_cast<std::size_t>(kExtent) * kExtent, word);
                GLuint texture = 0;
                glGenTextures(1, &texture);
                glBindTexture(GL_TEXTURE_2D, texture);
                glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(internalFormat), kExtent, kExtent, 0, GL_RGB, type,
                             words.data());
                // What Utils::makeTextureComplete does in the conformance cases, and what
                // glCopyImageSubData requires of both endpoints.
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glBindTexture(GL_TEXTURE_2D, 0);
                return texture;
            }

            // Every texel of level 0, as raw client words.
            std::vector<GLuint> ReadPackedWords(GLuint texture, GLenum type) {
                std::vector<GLuint> words(static_cast<std::size_t>(kExtent) * kExtent, 0xDEADBEEFu);
                glBindTexture(GL_TEXTURE_2D, texture);
                glGetTexImage(GL_TEXTURE_2D, 0, GL_RGB, type, words.data());
                glBindTexture(GL_TEXTURE_2D, 0);
                return words;
            }

            // The copy under test. Returns the error it raised so a driver that cannot perform the
            // move at all can skip rather than fail: the point of these cases is which BITS come
            // back, and there are none to compare if the copy never happened.
            GLenum CopyWholeImage(GLuint srcName, GLenum srcTarget, GLuint dstName, GLenum dstTarget) {
                DrainErrors();
                glCopyImageSubData(srcName, srcTarget, 0, 0, 0, 0, dstName, dstTarget, 0, 0, 0, 0, kExtent, kExtent,
                                   1);
                const GLenum error = glGetError();
                EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR)) << "the copy recorded more than one error";
                return error;
            }

            static void ExpectEveryTexel(const std::vector<GLuint>& words, GLuint expected, const char* what) {
                for (std::size_t i = 0; i < words.size(); ++i) {
                    ASSERT_EQ(words[i], expected)
                        << what << ": texel " << i << " read 0x" << std::hex << words[i] << ", expected 0x"
                        << expected;
                }
            }

            GLuint m_src = 0;
            GLuint m_dst = 0;
            GLuint m_rbo = 0;
        };

        // The control that has to hold before either regression means anything: a packed word
        // uploaded and read straight back must be the SAME word, not merely the same colour.
        TEST_F(PackedWordReadbackScenario, AnUploadedPackedWordReadsBackVerbatim) {
            if (!Ready()) GTEST_SKIP();

            m_src = MakePackedTexture(GL_RGB9_E5, GL_UNSIGNED_INT_5_9_9_9_REV, kRgb9E5Word);
            ASSERT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR)) << "RGB9_E5 upload";
            ExpectEveryTexel(ReadPackedWords(m_src, GL_UNSIGNED_INT_5_9_9_9_REV), kRgb9E5Word, "RGB9_E5 round trip");

            m_dst = MakePackedTexture(GL_R11F_G11F_B10F, GL_UNSIGNED_INT_10F_11F_11F_REV, kR11fG11fB10fWord);
            ASSERT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR)) << "R11F_G11F_B10F upload";
            ExpectEveryTexel(ReadPackedWords(m_dst, GL_UNSIGNED_INT_10F_11F_11F_REV), kR11fG11fB10fWord,
                             "R11F_G11F_B10F round trip");
        }

        // KHR-GL43.copy_image.functional rgb9_e5 -> r11f_g11f_b10f, all nine target combinations of
        // which failed on both GPUs. glCopyImageSubData is a raw block move, so the destination
        // physically holds the source's word - but the readback decoded it to float and re-encoded,
        // and the destination's blue field is a NaN whose payload float32 does not carry. Every
        // texel came back 0xf87c0000 (payload 1) instead of 0xf8fc0000 (payload 3): the same
        // "colour", two bits apart.
        TEST_F(PackedWordReadbackScenario, ACopiedRgb9E5WordSurvivesInAnR11fG11fB10fDestination) {
            if (!Ready()) GTEST_SKIP();

            m_src = MakePackedTexture(GL_RGB9_E5, GL_UNSIGNED_INT_5_9_9_9_REV, kRgb9E5Word);
            m_dst = MakePackedTexture(GL_R11F_G11F_B10F, GL_UNSIGNED_INT_10F_11F_11F_REV, 0u);
            ASSERT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR)) << "texture setup";

            const GLenum copyError = CopyWholeImage(m_src, GL_TEXTURE_2D, m_dst, GL_TEXTURE_2D);
            if (copyError != static_cast<GLenum>(GL_NO_ERROR)) {
                GTEST_SKIP() << "this driver declined the RGB9_E5 -> R11F_G11F_B10F copy (" << copyError << ")";
            }

            ExpectEveryTexel(ReadPackedWords(m_dst, GL_UNSIGNED_INT_10F_11F_11F_REV), kRgb9E5Word,
                             "copied word in the R11F_G11F_B10F destination");
            // ...and the source is still the source. This is verify()'s FIRST check in the
            // conformance case, and the half that a canonicalizing readback fails on its own.
            ExpectEveryTexel(ReadPackedWords(m_src, GL_UNSIGNED_INT_5_9_9_9_REV), kRgb9E5Word,
                             "the RGB9_E5 source after the copy");
        }

        // KHR-GL43.copy_image.functional *->rgb9_e5 with a GL_RENDERBUFFER source: exactly the three
        // renderbuffer combinations of each such family failed, and no texture one did. The
        // destination's CPU shadow is what the readback answered from, the mirror that replays a
        // copy into it declines when an endpoint is a renderbuffer (there is no shadow to mirror
        // FROM), and the decline is silent - so glGetTexImage handed back the destination's
        // pre-copy contents. The word chosen here makes that unmissable: it decodes to the same
        // all-zero channels the canonical encoder would write as 0x00000000.
        TEST_F(PackedWordReadbackScenario, ACopyThroughARenderbufferReachesAnRgb9E5Destination) {
            if (!Ready()) GTEST_SKIP();

            m_src = MakePackedTexture(GL_R11F_G11F_B10F, GL_UNSIGNED_INT_10F_11F_11F_REV, kR11fG11fB10fWord);
            m_dst = MakePackedTexture(GL_RGB9_E5, GL_UNSIGNED_INT_5_9_9_9_REV, 0xFFFFFFFFu);
            glGenRenderbuffers(1, &m_rbo);
            glBindRenderbuffer(GL_RENDERBUFFER, m_rbo);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_R11F_G11F_B10F, kExtent, kExtent);
            glBindRenderbuffer(GL_RENDERBUFFER, 0);
            ASSERT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR)) << "renderbuffer setup";

            // The conformance case's own shape: texture -> renderbuffer -> texture.
            const GLenum toRenderbuffer = CopyWholeImage(m_src, GL_TEXTURE_2D, m_rbo, GL_RENDERBUFFER);
            if (toRenderbuffer != static_cast<GLenum>(GL_NO_ERROR)) {
                GTEST_SKIP() << "this driver declined a renderbuffer copy destination (" << toRenderbuffer << ")";
            }
            const GLenum fromRenderbuffer = CopyWholeImage(m_rbo, GL_RENDERBUFFER, m_dst, GL_TEXTURE_2D);
            if (fromRenderbuffer != static_cast<GLenum>(GL_NO_ERROR)) {
                GTEST_SKIP() << "this driver declined a renderbuffer copy source (" << fromRenderbuffer << ")";
            }

            ExpectEveryTexel(ReadPackedWords(m_dst, GL_UNSIGNED_INT_5_9_9_9_REV), kR11fG11fB10fWord,
                             "copied word in the RGB9_E5 destination");
        }

    } // namespace
} // namespace MGITest
