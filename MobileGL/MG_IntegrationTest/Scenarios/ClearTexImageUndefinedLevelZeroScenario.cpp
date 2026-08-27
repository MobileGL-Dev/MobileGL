// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/ClearTexImageUndefinedLevelZeroScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - glClearTexImage ON A TEXTURE WHOSE GL LEVEL 0 WAS NEVER DEFINED.
//
// KHR-GL4[456].clear_tex_image.* builds exactly one shape: fillTexture() issues ONE
// glTexImage2D(GL_TEXTURE_2D, m_texLevel, ...) - the only texImage2D in the whole format/level
// family - sets GL_TEXTURE_MAX_LEVEL to that level, clears it and reads it back with
// glGetTexImage(..., m_texLevel, ...). For m_texLevel > 0 the levels BELOW the defined one have no
// storage at all, and the split in the conformance results was on that alone: every texLevel_0 body
// passed on DirectVulkan and every texLevel != 0 body failed, across all four internal formats and
// all three entry points.
//
// The frontend understands this shape - the clear is a pure CPU-shadow write, and
// ValidateTextureImageQuery deliberately does not demand mip completeness for a readback. The
// Vulkan backend did not: VkTextureManager takes storage mip 0 as the physical image extent, so a
// texture with no level 0 got no VkImage, SyncTextureAndGetDescriptor answered nullptr, and
// VulkanRenderer::GetTextureImage took a silent early return - leaving the caller's buffer exactly
// as it found it. The conformance failures carried no <Text> at all, because nothing raised a GL
// error: the destination was simply never written, so the test compared its own zero-initialized
// buffer against the clear value.
//
// The fix this pins is the readback fallback: with NO VkImage, nothing GPU-side can ever have
// written the texture, so the CPU shadow IS its content and is the correct answer. It is gated on
// "no image exists at all" and not on "syncing was inconvenient - a blanket shadow answer would
// return stale bytes for every render-to-texture result instead.
//
// NOT covered here, and deliberately: such a texture still has no VkImage, so it remains invisible
// to SAMPLING and rendering on DirectVulkan. Backing the image from the lowest defined level is a
// separate change (it moves every GL-level-to-subresource translation in the backend); this
// scenario asserts the readback contract only, and the DirectGLES leg - which has always been able
// to define a lone level N - is the built-in control for what the answer should be.

#include <array>
#include <cstdint>
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

        // The conformance family's own shape: a mid-chain level of a texture that has nothing else.
        constexpr GLint kDefinedLevel = 3;
        constexpr GLsizei kLevelExtent = 8;

        struct Texel8 {
            GLubyte r = 0, g = 0, b = 0, a = 0;
            bool operator==(const Texel8& other) const {
                return r == other.r && g == other.g && b == other.b && a == other.a;
            }
        };

        std::ostream& operator<<(std::ostream& os, const Texel8& c) {
            return os << "rgba(" << int(c.r) << "," << int(c.g) << "," << int(c.b) << "," << int(c.a) << ")";
        }

        // The conformance test's clear value is a single repeated component; 5 is what it uses, and
        // it is deliberately neither 0 (an unwritten destination) nor 255 (a saturated one).
        constexpr Texel8 kClearValue{5, 5, 5, 5};
        constexpr Texel8 kInitialValue{200, 100, 50, 255};

        class ClearTexImageUndefinedLevelZeroScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                DrainErrors();
            }

            void TearDown() override {
                if (!Ready()) return;
                if (m_texture != 0) {
                    glBindTexture(GL_TEXTURE_2D, 0);
                    glDeleteTextures(1, &m_texture);
                    m_texture = 0;
                }
                DrainErrors();
            }

            static void DrainErrors() {
                for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; ++i) {
                }
            }

            // One level and nothing else, through glTexImage2D - deliberately NOT glTexStorage2D,
            // which would define the whole chain and could not express "level 0 does not exist".
            void MakeTextureWithOnlyLevel(GLint level) {
                if (m_texture != 0) glDeleteTextures(1, &m_texture);
                glGenTextures(1, &m_texture);
                glBindTexture(GL_TEXTURE_2D, m_texture);
                const std::vector<Texel8> initial(static_cast<std::size_t>(kLevelExtent) * kLevelExtent, kInitialValue);
                glTexImage2D(GL_TEXTURE_2D, level, GL_RGBA8, kLevelExtent, kLevelExtent, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                             initial.data());
                // What the conformance case does: MAX_LEVEL names the one level that exists, and
                // BASE_LEVEL is left at its default 0 - which is what makes level 0 undefined AND
                // nominally the base level, the shape the backend could not express.
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, level);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                ASSERT_EQ(FirstGLError(), 0u) << "texture setup with only level " << level;
            }

            std::vector<Texel8> ReadLevel(GLint level) {
                std::vector<Texel8> pixels(static_cast<std::size_t>(kLevelExtent) * kLevelExtent, Texel8{0, 0, 0, 0});
                glBindTexture(GL_TEXTURE_2D, m_texture);
                glGetTexImage(GL_TEXTURE_2D, level, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
                EXPECT_EQ(FirstGLError(), 0u) << "glGetTexImage(level " << level << ") left a GL error behind";
                return pixels;
            }

            void ExpectAllTexels(const char* what, const std::vector<Texel8>& pixels, Texel8 expected) {
                std::size_t offenders = 0;
                Texel8 firstBad{};
                for (const Texel8& pixel : pixels) {
                    if (pixel == expected) continue;
                    if (offenders == 0) firstBad = pixel;
                    ++offenders;
                }
                EXPECT_EQ(offenders, 0u) << what << ": got " << firstBad << " instead of " << expected << " ("
                                         << offenders << " of " << pixels.size() << " texels wrong)";
            }

            GLuint m_texture = 0;
        };

    } // namespace

    // The regression. Before the fix glGetTexImage wrote nothing at all on DirectVulkan, so the
    // caller's buffer kept whatever it already held - which is why the conformance failures showed
    // the test's own zero-initialized memory and carried no GL error.
    TEST_F(ClearTexImageUndefinedLevelZeroScenario, ClearAndReadBackALevelWhoseLowerLevelsDoNotExist) {
        if (!Ready()) GTEST_SKIP();
        MakeTextureWithOnlyLevel(kDefinedLevel);

        // Pre-flight: the level reads back as what was uploaded. This is what makes the assertion
        // after the clear falsifiable - without it, a readback that silently wrote nothing could not
        // be told from one that wrote the right answer.
        ExpectAllTexels("before the clear", ReadLevel(kDefinedLevel), kInitialValue);

        glClearTexImage(m_texture, kDefinedLevel, GL_RGBA, GL_UNSIGNED_BYTE, &kClearValue);
        EXPECT_EQ(FirstGLError(), 0u) << "glClearTexImage was rejected";

        ExpectAllTexels("after the clear", ReadLevel(kDefinedLevel), kClearValue);
        Gl().EndFrame();
    }

    // The same shape through glClearTexSubImage, which is a separate entry point in the conformance
    // family and failed on exactly the same bodies.
    TEST_F(ClearTexImageUndefinedLevelZeroScenario, ClearSubImageOfALevelWhoseLowerLevelsDoNotExist) {
        if (!Ready()) GTEST_SKIP();
        MakeTextureWithOnlyLevel(kDefinedLevel);

        glClearTexSubImage(m_texture, kDefinedLevel, 0, 0, 0, kLevelExtent, kLevelExtent, 1, GL_RGBA,
                           GL_UNSIGNED_BYTE, &kClearValue);
        EXPECT_EQ(FirstGLError(), 0u) << "glClearTexSubImage was rejected";

        ExpectAllTexels("after the sub-image clear", ReadLevel(kDefinedLevel), kClearValue);
        Gl().EndFrame();
    }

    // The negative control: an ORDINARY texture, whose level 0 does exist, must keep answering from
    // the GPU image rather than being diverted onto the shadow. A fallback that fired unconditionally
    // would pass the two tests above and this one too - but it would also hand back stale bytes for
    // anything the GPU had written, which is why the partial-clear check below matters: the readback
    // has to see a region the backend cleared and a region it did not, in one image.
    TEST_F(ClearTexImageUndefinedLevelZeroScenario, AnOrdinaryLevelZeroTextureStillReadsBackCorrectly) {
        if (!Ready()) GTEST_SKIP();
        MakeTextureWithOnlyLevel(0);

        ExpectAllTexels("before the clear", ReadLevel(0), kInitialValue);

        // Clear only the left half, so the answer is neither "all initial" nor "all cleared".
        glClearTexSubImage(m_texture, 0, 0, 0, 0, kLevelExtent / 2, kLevelExtent, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                           &kClearValue);
        EXPECT_EQ(FirstGLError(), 0u) << "glClearTexSubImage was rejected";

        const std::vector<Texel8> pixels = ReadLevel(0);
        ASSERT_EQ(pixels.size(), static_cast<std::size_t>(kLevelExtent) * kLevelExtent);
        for (int y = 0; y < kLevelExtent; ++y) {
            for (int x = 0; x < kLevelExtent; ++x) {
                const Texel8 expected = x < kLevelExtent / 2 ? kClearValue : kInitialValue;
                const Texel8 actual = pixels[static_cast<std::size_t>(y) * kLevelExtent + x];
                ASSERT_EQ(actual, expected) << "at (" << x << "," << y << ")";
            }
        }
        Gl().EndFrame();
    }

} // namespace MGITest
