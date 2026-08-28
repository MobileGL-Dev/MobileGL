// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/CopyImagePacked16Scenario.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - glCopyImageSubData PRESERVES 16-BIT PACKED WORDS ACROSS AN ARRAY MIP LEVEL.
//
// The shape is lifted verbatim from the 18 Espryt bodies of KHR-GL4x.copy_image.functional
// that survived every earlier wave: the three internal formats MobileGL can keep as 16-bit
// packed ES storage - GL_RGB5 (stored GL_RGB565), GL_RGB5_A1, GL_RGBA4 - crossed with the
// target pairs that put a GL_TEXTURE_2D_ARRAY's MIP LEVEL 1 on one side of the copy. On the
// affected Mali the driver's physical field order for such a level is the *_REV mirror of
// every other image's, and glCopyImageSubData - a raw texel-block move - lands the fields
// reversed: src word 0x0047 arrives as 0x8C20 (its 5_5_5_1 -> 1_5_5_5_REV re-encoding),
// 0x0007 as 0x3800, byte-exact on every failing body. Uploads and readbacks of the same
// level are clean (the driver decodes its own layout consistently), which is why only the
// copy path ever crossed the two layouts and why the CTS's "source image was not modified"
// checks always passed.
//
// The array is 30x30x12 with THREE levels and the flat endpoint is 7x7 with three levels
// (7/3/1) because that is the allocation the failures pin - the CTS builds every functional
// texture with FUNCTIONAL_TEST_N_LEVELS = 3 (makeTextureComplete(0, 2)) - and the same
// suite's level-0 copies and a 14x14 base's level 1 measured clean on the same driver, so
// any deviation from the measured shape (a smaller array, a shorter chain) might sit on the
// clean side of whatever allocation threshold picks the driver's layout.
//
// The repair under test is the packed16 storage widening
// (PixelFormatNormalizeOptionBit::WidenPacked16Norm): where the POST probe
// (SelfTest::CopyImageMirrorsPacked16FieldOrder) measures the mirror - or
// MOBILEGL_WIDEN_PACKED16_STORAGE forces it - the three formats are stored as
// GL_RGB8/GL_RGBA8, leaving no 16-bit packed image for a copy to disagree about. The client
// word still round-trips exactly: the canonical shadow is already UNorm8, and an n-bit field
// encodes to UNorm8 and back losslessly for every n <= 8.
//
// This scenario runs in BOTH configurations, and both must hand back identical client words:
//   * the ambient registrations take the narrow path on a clean driver (llvmpipe has no
//     mirror, so Auto keeps the native 16-bit storage - the pre-existing behaviour stays
//     covered);
//   * the DirectGLES.WidenedPacked16. registration pins MOBILEGL_WIDEN_PACKED16_STORAGE=1,
//     which is the storage every affected device will actually run - without it the repair
//     is unfalsifiable off-device, because no CI driver has the bug that arms it.
// The Mali mirror itself CANNOT be reproduced here; only the on-device CTS run can show the
// widening killing the 18 bodies. What this scenario pins is that the widened storage is
// client-invisible: same words in, same words out, on every leg the failing bodies used.
//
// DirectVulkan is the control - Magma has always resolved these formats to RGBA8 - so a
// failure on both backends means the scenario is wrong, and a failure on DirectGLES alone
// means the widening (or the narrow path it replaces) is.

#include <algorithm>
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

        constexpr int kBaseSize = 30; // array level 0; level 1 is 15x15
        constexpr int kLevel1Size = kBaseSize / 2;
        constexpr int kLayers = 12;
        constexpr int kFlatSize = 7; // the plain-2D / renderbuffer endpoint, level 0
        // Copies cover the whole flat endpoint and land at (8, 8) inside the 15x15 level so
        // that offsets are honoured, not just texel (0, 0): 8 + 7 == 15 reaches the far edge.
        constexpr int kRegion = kFlatSize;
        constexpr int kArrayOffset = 8;

        struct PackedFormatCase {
            GLenum internalFormat; // the spelling the CTS uses
            GLenum transferFormat;
            GLenum transferType;
            const char* name;
        };

        // Per-texel varying words, every field inside its width, so a swapped field order (or
        // a mis-addressed row) cannot cancel out the way a uniform fill would let it.
        GLushort MakeWord(GLenum type, int i) {
            switch (type) {
            case GL_UNSIGNED_SHORT_5_6_5: {
                const int r = i % 32, g = (i * 7 + 3) % 64, b = (i * 5 + 11) % 32;
                return static_cast<GLushort>((r << 11) | (g << 5) | b);
            }
            case GL_UNSIGNED_SHORT_4_4_4_4: {
                const int r = i % 16, g = (i * 3 + 1) % 16, b = (i * 7 + 5) % 16, a = (i * 5 + 2) % 16;
                return static_cast<GLushort>((r << 12) | (g << 8) | (b << 4) | a);
            }
            case GL_UNSIGNED_SHORT_5_5_5_1: {
                const int r = i % 32, g = (i * 7 + 3) % 32, b = (i * 3 + 11) % 32, a = i % 2;
                return static_cast<GLushort>((r << 11) | (g << 6) | (b << 1) | a);
            }
            default:
                return 0;
            }
        }

        std::vector<GLushort> MakeWords(GLenum type, int count, int seed) {
            std::vector<GLushort> words(static_cast<size_t>(count));
            for (int i = 0; i < count; ++i) {
                words[static_cast<size_t>(i)] = MakeWord(type, i + seed);
            }
            return words;
        }

        class CopyImagePacked16Scenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                // 16-bit rows are 2-byte aligned; the default 4-byte row alignment would pad
                // every odd-width row of the 15x15 level and shear the comparisons.
                glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
                glPixelStorei(GL_PACK_ALIGNMENT, 2);
                if (!CopyImageSubDataUsable()) {
                    GTEST_SKIP() << "glCopyImageSubData is unavailable on backend " << Gl().BackendName();
                }
            }

            void TearDown() override {
                if (!Ready()) return;
                glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
                glPixelStorei(GL_PACK_ALIGNMENT, 4);
                for (const GLuint texture : m_textures) {
                    glDeleteTextures(1, &texture);
                }
                m_textures.clear();
                if (m_renderbuffer != 0) {
                    glDeleteRenderbuffers(1, &m_renderbuffer);
                    m_renderbuffer = 0;
                }
                if (m_fbo != 0) {
                    glBindFramebuffer(GL_FRAMEBUFFER, 0);
                    glDeleteFramebuffers(1, &m_fbo);
                    m_fbo = 0;
                }
            }

            bool CopyImageSubDataUsable() {
                GLuint probe[2] = {0, 0};
                glGenTextures(2, probe);
                for (const GLuint texture : probe) {
                    glBindTexture(GL_TEXTURE_2D_ARRAY, texture);
                    glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_RGBA8, 1, 1, 1);
                }
                glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
                while (glGetError() != GL_NO_ERROR) {
                }
                glCopyImageSubData(probe[0], GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, probe[1], GL_TEXTURE_2D_ARRAY, 0, 0, 0,
                                   0, 1, 1, 1);
                const bool usable = glGetError() == GL_NO_ERROR;
                glDeleteTextures(2, probe);
                return usable;
            }

            // The CTS's own mutable shape: glTexImage3D per level, filter NEAREST, THREE levels
            // (30/15/7) with the chain clamped to them. Level 2 carries its own fill so nothing
            // below can pass by reading a level that was never written.
            GLuint MakeArrayTexture(const PackedFormatCase& format, const std::vector<GLushort>& level0,
                                    const std::vector<GLushort>& level1) {
                GLuint texture = 0;
                glGenTextures(1, &texture);
                m_textures.push_back(texture);
                glBindTexture(GL_TEXTURE_2D_ARRAY, texture);
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, 2);
                glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, static_cast<GLint>(format.internalFormat), kBaseSize, kBaseSize,
                             kLayers, 0, format.transferFormat, format.transferType, level0.data());
                glTexImage3D(GL_TEXTURE_2D_ARRAY, 1, static_cast<GLint>(format.internalFormat), kLevel1Size,
                             kLevel1Size, kLayers, 0, format.transferFormat, format.transferType, level1.data());
                const int level2Size = kLevel1Size / 2;
                const auto level2 = MakeWords(format.transferType, level2Size * level2Size * kLayers, 211);
                glTexImage3D(GL_TEXTURE_2D_ARRAY, 2, static_cast<GLint>(format.internalFormat), level2Size,
                             level2Size, kLayers, 0, format.transferFormat, format.transferType, level2.data());
                glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
                return texture;
            }

            // Three levels (7/3/1) like the CTS's plain endpoints; `texels` is level 0, the one
            // every assertion reads.
            GLuint MakeFlatTexture(const PackedFormatCase& format, const std::vector<GLushort>& texels) {
                GLuint texture = 0;
                glGenTextures(1, &texture);
                m_textures.push_back(texture);
                glBindTexture(GL_TEXTURE_2D, texture);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 2);
                glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(format.internalFormat), kFlatSize, kFlatSize, 0,
                             format.transferFormat, format.transferType, texels.data());
                for (int level = 1; level <= 2; ++level) {
                    const int size = std::max(kFlatSize >> level, 1);
                    const auto fill = MakeWords(format.transferType, size * size, 97 + level);
                    glTexImage2D(GL_TEXTURE_2D, level, static_cast<GLint>(format.internalFormat), size, size, 0,
                                 format.transferFormat, format.transferType, fill.data());
                }
                glBindTexture(GL_TEXTURE_2D, 0);
                return texture;
            }

            std::vector<GLushort> ReadTexImage(GLenum target, GLuint texture, int level,
                                               const PackedFormatCase& format, size_t texelCount) {
                std::vector<GLushort> words(texelCount, 0);
                glBindTexture(target, texture);
                glGetTexImage(target, level, format.transferFormat, format.transferType, words.data());
                glBindTexture(target, 0);
                return words;
            }

            // Every word of `got` inside the kRegion-square at (x0, y0) of a width-wide layer-0
            // image equals the corresponding source word, and every word outside it still holds
            // `fill`'s. Failures name the texel and both words, which is what turns a field-order
            // regression into a one-line diagnosis.
            void ExpectRegion(const std::vector<GLushort>& got, int width, int x0, int y0,
                              const std::vector<GLushort>& source, int sourceWidth, int sourceX0, int sourceY0,
                              const std::vector<GLushort>& fill, const char* what) {
                for (int y = 0; y < width; ++y) {
                    for (int x = 0; x < width && static_cast<size_t>(y * width + x) < got.size(); ++x) {
                        const bool inRegion =
                            x >= x0 && x < x0 + kRegion && y >= y0 && y < y0 + kRegion;
                        const GLushort actual = got[static_cast<size_t>(y * width + x)];
                        const GLushort expected =
                            inRegion ? source[static_cast<size_t>((sourceY0 + y - y0) * sourceWidth + sourceX0 +
                                                                  (x - x0))]
                                     : fill[static_cast<size_t>(y * width + x)];
                        EXPECT_EQ(actual, expected)
                            << what << ": texel (" << x << ", " << y << ")"
                            << (inRegion ? " (copied)" : " (untouched)") << " holds 0x" << std::hex << actual
                            << ", expected 0x" << expected;
                        if (actual != expected) return; // one texel names the defect; 224 more would bury it
                    }
                }
            }

            std::vector<GLuint> m_textures;
            GLuint m_renderbuffer = 0;
            GLuint m_fbo = 0;
        };

        const PackedFormatCase kFormats[] = {
            {GL_RGB5, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, "rgb5"},
            {GL_RGB5_A1, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, "rgb5_a1"},
            {GL_RGBA4, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, "rgba4"},
        };

        // texture_2d (the ES image behind GL_TEXTURE_RECTANGLE too) -> the array's level 1:
        // the array-as-destination direction of 12 of the 18 failing bodies.
        TEST_F(CopyImagePacked16Scenario, FlatImageLandsInArrayMipLevelIntact) {
            if (!Ready() || IsSkipped()) return;
            for (const PackedFormatCase& format : kFormats) {
                const auto level0 = MakeWords(format.transferType, kBaseSize * kBaseSize * kLayers, 1);
                const auto level1 = MakeWords(format.transferType, kLevel1Size * kLevel1Size * kLayers, 7);
                const auto flat = MakeWords(format.transferType, kFlatSize * kFlatSize, 131);
                const GLuint array = MakeArrayTexture(format, level0, level1);
                const GLuint source = MakeFlatTexture(format, flat);
                ASSERT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR)) << format.name << ": setup failed";

                glCopyImageSubData(source, GL_TEXTURE_2D, 0, 0, 0, 0, array, GL_TEXTURE_2D_ARRAY, 1, kArrayOffset,
                                   kArrayOffset, 0, kRegion, kRegion, 1);
                ASSERT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR))
                    << format.name << ": glCopyImageSubData raised an error";

                const auto got = ReadTexImage(GL_TEXTURE_2D_ARRAY, array, 1, format,
                                              static_cast<size_t>(kLevel1Size) * kLevel1Size * kLayers);
                ExpectRegion(got, kLevel1Size, kArrayOffset, kArrayOffset, flat, kFlatSize, 0, 0, level1,
                             (std::string("2d->2d_array level 1, ") + format.name).c_str());
                // The source must not have moved - the CTS asserts this before it ever looks at
                // the destination, and it is what pins the corruption to the copy itself.
                const auto sourceAfter =
                    ReadTexImage(GL_TEXTURE_2D, source, 0, format, static_cast<size_t>(kFlatSize) * kFlatSize);
                ExpectRegion(sourceAfter, kFlatSize, 0, 0, flat, kFlatSize, 0, 0, flat,
                             (std::string("source after 2d->2d_array, ") + format.name).c_str());
            }
        }

        // The array's level 1 -> texture_2d: the array-as-source direction of the other 6
        // bodies (2d_array -> 3d and 2d_array -> rectangle both read the level-1 array).
        TEST_F(CopyImagePacked16Scenario, ArrayMipLevelLandsInFlatImageIntact) {
            if (!Ready() || IsSkipped()) return;
            for (const PackedFormatCase& format : kFormats) {
                const auto level0 = MakeWords(format.transferType, kBaseSize * kBaseSize * kLayers, 1);
                const auto level1 = MakeWords(format.transferType, kLevel1Size * kLevel1Size * kLayers, 7);
                const auto fill = MakeWords(format.transferType, kFlatSize * kFlatSize, 131);
                const GLuint array = MakeArrayTexture(format, level0, level1);
                const GLuint destination = MakeFlatTexture(format, fill);
                ASSERT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR)) << format.name << ": setup failed";

                glCopyImageSubData(array, GL_TEXTURE_2D_ARRAY, 1, kArrayOffset, kArrayOffset, 0, destination,
                                   GL_TEXTURE_2D, 0, 0, 0, 0, kRegion, kRegion, 1);
                ASSERT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR))
                    << format.name << ": glCopyImageSubData raised an error";

                const auto got = ReadTexImage(GL_TEXTURE_2D, destination, 0, format,
                                              static_cast<size_t>(kFlatSize) * kFlatSize);
                ExpectRegion(got, kFlatSize, 0, 0, level1, kLevel1Size, kArrayOffset, kArrayOffset, fill,
                             (std::string("2d_array level 1 -> 2d, ") + format.name).c_str());
            }
        }

        // renderbuffer -> the array's level 1: the leg the remaining 3 bodies use, and the one
        // that requires the renderbuffer's ES storage to move together with the textures' -
        // glCopyImageSubData needs both endpoints in the same driver format, so a widening that
        // reached textures alone would break exactly here.
        TEST_F(CopyImagePacked16Scenario, RenderbufferLandsInArrayMipLevelIntact) {
            if (!Ready() || IsSkipped()) return;
            for (const PackedFormatCase& format : kFormats) {
                const auto level0 = MakeWords(format.transferType, kBaseSize * kBaseSize * kLayers, 1);
                const auto level1 = MakeWords(format.transferType, kLevel1Size * kLevel1Size * kLayers, 7);
                const GLuint array = MakeArrayTexture(format, level0, level1);

                if (m_renderbuffer == 0) glGenRenderbuffers(1, &m_renderbuffer);
                glBindRenderbuffer(GL_RENDERBUFFER, m_renderbuffer);
                glRenderbufferStorage(GL_RENDERBUFFER, format.internalFormat, kFlatSize, kFlatSize);
                if (m_fbo == 0) glGenFramebuffers(1, &m_fbo);
                glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
                glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, m_renderbuffer);
                ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE))
                    << format.name << ": the renderbuffer is not attachable";
                // Field values picked to encode exactly in the narrow fields AND in their
                // UNorm8 expansions, so the expected word is the same whichever storage the
                // configuration picked - which is the point of the whole scenario.
                const int maxG = format.transferType == GL_UNSIGNED_SHORT_5_6_5 ? 63 : 31;
                const int max = format.transferType == GL_UNSIGNED_SHORT_4_4_4_4 ? 15 : 31;
                const int maxGreen = format.transferType == GL_UNSIGNED_SHORT_4_4_4_4 ? 15 : maxG;
                const GLfloat clearColor[4] = {static_cast<GLfloat>(8 % (max + 1)) / max,
                                               static_cast<GLfloat>(maxGreen / 2) / maxGreen,
                                               static_cast<GLfloat>(max - 2) / max, 1.0f};
                // The context is shared with every scenario in this process; a scissor left on
                // would clip the clear and hand the copy undefined renderbuffer texels.
                glDisable(GL_SCISSOR_TEST);
                glClearBufferfv(GL_COLOR, 0, clearColor);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                ASSERT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR)) << format.name << ": setup failed";

                glCopyImageSubData(m_renderbuffer, GL_RENDERBUFFER, 0, 0, 0, 0, array, GL_TEXTURE_2D_ARRAY, 1,
                                   kArrayOffset, kArrayOffset, 0, kRegion, kRegion, 1);
                ASSERT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR))
                    << format.name << ": glCopyImageSubData raised an error";

                GLushort clearedWord = 0;
                switch (format.transferType) {
                case GL_UNSIGNED_SHORT_5_6_5:
                    clearedWord = static_cast<GLushort>((8 << 11) | ((maxGreen / 2) << 5) | (max - 2));
                    break;
                case GL_UNSIGNED_SHORT_5_5_5_1:
                    clearedWord = static_cast<GLushort>((8 << 11) | ((maxGreen / 2) << 6) | ((max - 2) << 1) | 1);
                    break;
                case GL_UNSIGNED_SHORT_4_4_4_4:
                    clearedWord = static_cast<GLushort>((8 << 12) | ((maxGreen / 2) << 8) | ((max - 2) << 4) | 15);
                    break;
                default:
                    break;
                }
                std::vector<GLushort> expectedRegion(static_cast<size_t>(kRegion) * kRegion, clearedWord);
                const auto got = ReadTexImage(GL_TEXTURE_2D_ARRAY, array, 1, format,
                                              static_cast<size_t>(kLevel1Size) * kLevel1Size * kLayers);
                ExpectRegion(got, kLevel1Size, kArrayOffset, kArrayOffset, expectedRegion, kRegion, 0, 0, level1,
                             (std::string("renderbuffer -> 2d_array level 1, ") + format.name).c_str());
            }
        }

    } // namespace
} // namespace MGITest
