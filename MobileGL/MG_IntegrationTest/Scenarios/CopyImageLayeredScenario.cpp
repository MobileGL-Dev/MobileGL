// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/CopyImageLayeredScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - glCopyImageSubData MOVES EVERY SLICE IT WAS ASKED FOR, NOT JUST SLICE 0.
//
// KHR-GL43.copy_image.functional_* copies a whole 12-layer region in one call whenever both
// endpoints are layered, i.e. for the four target pairs 2d_array->2d_array, 2d_array->3d,
// 3d->2d_array and 3d->3d. DirectVulkan built its VkImageCopy with baseArrayLayer 0, layerCount 1
// and srcOffset.z 0 no matter what the call asked for, so slice 0 landed correctly and slices 1..N
// were never written - 64 conformance cases (16 compatible format pairs x those 4 pairs) failing
// with "first mismatch at [x, y, 1]", the first texel of the first slice the copy skipped.
//
// The reason one hardcode covered both shapes wrongly is that GL states a layered copy ONE way -
// srcZ/dstZ and srcDepth - while Vulkan states it two ways and picks by image type:
//
//   GL_TEXTURE_3D       -> VK_IMAGE_TYPE_3D: slices are z, so srcOffset.z/dstOffset.z select them
//                          and extent.depth counts them; the layer range must stay (0, 1).
//   GL_TEXTURE_2D_ARRAY -> VK_IMAGE_TYPE_2D: slices are array layers, so baseArrayLayer selects
//                          them and layerCount counts them; offset.z stays 0.
//
// A mixed pair is legal (maintenance1, core in Vulkan 1.1) but only when the counts correspond:
// the 3D side's extent.depth has to equal the array side's layerCount. So the four pairs below are
// four DIFFERENT VkImageCopy shapes, not one shape with different arguments, which is why one
// scenario per pair is the coverage that matters here.
//
// Every case also asserts the slices OUTSIDE the copied range still hold their fill. A backend
// that "fixed" the miss by copying the whole image regardless of srcZ/srcDepth would pass a
// slices-landed check and fail this one.
//
// The verification path is an FBO attachment per slice plus glReadPixels, not glGetTexImage: it is
// the readback both backends share, and glFramebufferTextureLayer names an array layer and a 3D
// slice through the same call, so the two texture kinds are read back identically.
//
// DirectGLES is the control - it forwards to the driver's own glCopyImageSubData - so a failure on
// both backends means the scenario is wrong, and a failure on DirectVulkan alone means Magma is.

#include <array>
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

        constexpr int kWidth = 4;
        constexpr int kHeight = 4;
        // Six is enough for a copy that starts and ends away from both edges of both endpoints
        // while still leaving untouched slices on either side to assert against.
        constexpr int kSlices = 6;

        struct Rgba8 {
            GLubyte r = 0, g = 0, b = 0, a = 0;

            bool operator==(const Rgba8& other) const {
                return r == other.r && g == other.g && b == other.b && a == other.a;
            }
        };

        std::string Describe(const Rgba8& color) {
            return "(" + std::to_string(color.r) + ", " + std::to_string(color.g) + ", " + std::to_string(color.b) +
                   ", " + std::to_string(color.a) + ")";
        }

        // Per-slice constants, uniform within a slice. A uniform fill is deliberate: the defect is
        // in which SLICE the copy addresses, and a value that also varied within the slice would
        // make the assertions depend on the framebuffer row order as well.
        Rgba8 SourceColor(int slice) {
            return {static_cast<GLubyte>(10 + slice * 20), static_cast<GLubyte>(40 + slice * 10),
                    static_cast<GLubyte>(200 - slice * 15), 255};
        }

        Rgba8 DestinationFill(int slice) {
            return {static_cast<GLubyte>(3 + slice), static_cast<GLubyte>(250 - slice * 7),
                    static_cast<GLubyte>(120 + slice * 5), 255};
        }

        class CopyImageLayeredScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                if (!CopyImageSubDataUsable()) {
                    GTEST_SKIP() << "glCopyImageSubData is unavailable on backend " << Gl().BackendName();
                }
            }

            void TearDown() override {
                if (!Ready()) return;
                for (const GLuint texture : m_textures) {
                    glDeleteTextures(1, &texture);
                }
                m_textures.clear();
                if (m_fbo != 0) {
                    glBindFramebuffer(GL_FRAMEBUFFER, 0);
                    glDeleteFramebuffers(1, &m_fbo);
                    m_fbo = 0;
                }
            }

            // A trivial 1x1x1 array-to-array copy: it exercises the entry point without depending
            // on any of the behaviour under test, so a driver (or a backend function table) that
            // simply does not have the call skips instead of failing every case below.
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

            // `target` is GL_TEXTURE_2D_ARRAY or GL_TEXTURE_3D; both take glTexStorage3D and
            // glTexSubImage3D with the slice on the same axis, which is the whole reason GL can
            // copy between them. `levels` > 1 puts a real mip chain behind the level the copy
            // names, so the level's own extent - a 3D level's depth included - has to be resolved
            // rather than assumed to be the image's.
            GLuint MakeTexture(GLenum target, int levels, Rgba8 (*colorForSlice)(int)) {
                GLuint texture = 0;
                glGenTextures(1, &texture);
                m_textures.push_back(texture);
                glBindTexture(target, texture);
                glTexStorage3D(target, levels, GL_RGBA8, kWidth << (levels - 1), kHeight << (levels - 1),
                               target == GL_TEXTURE_3D ? (kSlices << (levels - 1)) : kSlices);
                glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

                // Fill every level, so nothing below can pass by reading a level that was never
                // written and happened to hold the expected bytes.
                for (int level = 0; level < levels; ++level) {
                    const int levelWidth = kWidth << (levels - 1 - level);
                    const int levelHeight = kHeight << (levels - 1 - level);
                    const int levelSlices =
                        target == GL_TEXTURE_3D ? (kSlices << (levels - 1 - level)) : kSlices;
                    for (int slice = 0; slice < levelSlices; ++slice) {
                        const Rgba8 color = colorForSlice(slice % kSlices);
                        std::vector<Rgba8> texels(static_cast<size_t>(levelWidth) * levelHeight, color);
                        glTexSubImage3D(target, level, 0, 0, slice, levelWidth, levelHeight, 1, GL_RGBA,
                                        GL_UNSIGNED_BYTE, texels.data());
                    }
                }
                glBindTexture(target, 0);
                return texture;
            }

            // One slice of one level, through an FBO attachment. glFramebufferTextureLayer takes an
            // array layer and a 3D slice through the same argument, so both targets read back the
            // same way.
            Rgba8 ReadSlice(GLuint texture, int level, int slice, int width, int height) {
                if (m_fbo == 0) {
                    glGenFramebuffers(1, &m_fbo);
                }
                glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
                glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, texture, level, slice);
                EXPECT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE))
                    << "slice " << slice << " of level " << level << " is not attachable";
                std::vector<Rgba8> pixels(static_cast<size_t>(width) * height, Rgba8{});
                glReadBuffer(GL_COLOR_ATTACHMENT0);
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
                glBindFramebuffer(GL_FRAMEBUFFER, 0);

                // The fill is uniform within a slice, so any disagreement between texels is itself
                // a failure - reported here rather than silently reduced to pixels[0].
                for (size_t i = 1; i < pixels.size(); ++i) {
                    EXPECT_TRUE(pixels[i] == pixels[0])
                        << "slice " << slice << " of level " << level << " is not uniform: texel 0 is "
                        << Describe(pixels[0]) << ", texel " << i << " is " << Describe(pixels[i]);
                }
                return pixels[0];
            }

            // The assertion every case ends with: slices inside [dstZ, dstZ + depth) hold the
            // source slice they were fed, and every slice outside it still holds its own fill.
            void ExpectCopied(GLuint destination, int level, int width, int height, int sliceCount, int srcZ,
                              int dstZ, int depth, const char* what) {
                for (int slice = 0; slice < sliceCount; ++slice) {
                    const bool inRange = slice >= dstZ && slice < dstZ + depth;
                    const Rgba8 expected =
                        inRange ? SourceColor(srcZ + (slice - dstZ)) : DestinationFill(slice);
                    const Rgba8 actual = ReadSlice(destination, level, slice, width, height);
                    EXPECT_TRUE(actual == expected)
                        << what << ": destination slice " << slice << (inRange ? " (copied)" : " (untouched)")
                        << " is " << Describe(actual) << ", expected " << Describe(expected);
                }
            }

            std::vector<GLuint> m_textures;
            GLuint m_fbo = 0;
        };

        // 2d_array -> 2d_array. Both endpoints put the slices on the layer axis, so BOTH layer
        // counts carry the depth and extent.depth must stay 1.
        TEST_F(CopyImageLayeredScenario, ArrayToArrayCopiesEverySlice) {
            if (!Ready() || IsSkipped()) return;

            const GLuint source = MakeTexture(GL_TEXTURE_2D_ARRAY, 1, SourceColor);
            const GLuint destination = MakeTexture(GL_TEXTURE_2D_ARRAY, 1, DestinationFill);
            ASSERT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR)) << "texture setup failed";

            glCopyImageSubData(source, GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, destination, GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0,
                               kWidth, kHeight, kSlices);
            ASSERT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR)) << "glCopyImageSubData raised an error";

            ExpectCopied(destination, 0, kWidth, kHeight, kSlices, 0, 0, kSlices, "array->array, all slices");
        }

        // The same pair with the layer ranges offset differently on the two sides: the shape that
        // separates "copies more than slice 0" from "copies the RIGHT slices". A backend that read
        // the source range but wrote from layer 0 (or vice versa) passes the case above.
        TEST_F(CopyImageLayeredScenario, ArrayToArrayHonoursDifferentLayerOffsets) {
            if (!Ready() || IsSkipped()) return;

            const GLuint source = MakeTexture(GL_TEXTURE_2D_ARRAY, 1, SourceColor);
            const GLuint destination = MakeTexture(GL_TEXTURE_2D_ARRAY, 1, DestinationFill);
            ASSERT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR)) << "texture setup failed";

            constexpr int kSrcZ = 3;
            constexpr int kDstZ = 1;
            constexpr int kDepth = 2;
            glCopyImageSubData(source, GL_TEXTURE_2D_ARRAY, 0, 0, 0, kSrcZ, destination, GL_TEXTURE_2D_ARRAY, 0, 0, 0,
                               kDstZ, kWidth, kHeight, kDepth);
            ASSERT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR)) << "glCopyImageSubData raised an error";

            ExpectCopied(destination, 0, kWidth, kHeight, kSlices, kSrcZ, kDstZ, kDepth,
                         "array->array, offset layer ranges");
        }

        // 3d -> 3d. Neither endpoint has array layers at all: the depth travels on extent.depth and
        // the offsets on srcOffset.z/dstOffset.z, with both layer counts pinned to 1.
        TEST_F(CopyImageLayeredScenario, VolumeToVolumeHonoursNonZeroZ) {
            if (!Ready() || IsSkipped()) return;

            const GLuint source = MakeTexture(GL_TEXTURE_3D, 1, SourceColor);
            const GLuint destination = MakeTexture(GL_TEXTURE_3D, 1, DestinationFill);
            ASSERT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR)) << "texture setup failed";

            constexpr int kSrcZ = 1;
            constexpr int kDstZ = 3;
            constexpr int kDepth = 3;
            glCopyImageSubData(source, GL_TEXTURE_3D, 0, 0, 0, kSrcZ, destination, GL_TEXTURE_3D, 0, 0, 0, kDstZ,
                               kWidth, kHeight, kDepth);
            ASSERT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR)) << "glCopyImageSubData raised an error";

            ExpectCopied(destination, 0, kWidth, kHeight, kSlices, kSrcZ, kDstZ, kDepth, "3d->3d, non-zero z");
        }

        // The same pair one mip level down. A 3D level's DEPTH halves with its width and height, so
        // this is the only case where the slice count the copy may name is not the image's own -
        // the bound a layered endpoint is checked against has to come from the level.
        TEST_F(CopyImageLayeredScenario, VolumeToVolumeAtNonZeroMipLevel) {
            if (!Ready() || IsSkipped()) return;

            const GLuint source = MakeTexture(GL_TEXTURE_3D, 2, SourceColor);
            const GLuint destination = MakeTexture(GL_TEXTURE_3D, 2, DestinationFill);
            ASSERT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR)) << "texture setup failed";

            constexpr int kLevel = 1;
            constexpr int kSrcZ = 2;
            constexpr int kDstZ = 0;
            constexpr int kDepth = 4;
            glCopyImageSubData(source, GL_TEXTURE_3D, kLevel, 0, 0, kSrcZ, destination, GL_TEXTURE_3D, kLevel, 0, 0,
                               kDstZ, kWidth, kHeight, kDepth);
            ASSERT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR)) << "glCopyImageSubData raised an error";

            ExpectCopied(destination, kLevel, kWidth, kHeight, kSlices, kSrcZ, kDstZ, kDepth,
                         "3d->3d at mip level 1");
        }

        // 2d_array -> 3d. The mixed shape: the source counts its slices as layers, the destination
        // as depth, and Vulkan requires extent.depth to equal the source's layerCount.
        TEST_F(CopyImageLayeredScenario, ArrayToVolumeCopiesEverySlice) {
            if (!Ready() || IsSkipped()) return;

            const GLuint source = MakeTexture(GL_TEXTURE_2D_ARRAY, 1, SourceColor);
            const GLuint destination = MakeTexture(GL_TEXTURE_3D, 1, DestinationFill);
            ASSERT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR)) << "texture setup failed";

            constexpr int kSrcZ = 2;
            constexpr int kDstZ = 1;
            constexpr int kDepth = 4;
            glCopyImageSubData(source, GL_TEXTURE_2D_ARRAY, 0, 0, 0, kSrcZ, destination, GL_TEXTURE_3D, 0, 0, 0, kDstZ,
                               kWidth, kHeight, kDepth);
            ASSERT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR)) << "glCopyImageSubData raised an error";

            ExpectCopied(destination, 0, kWidth, kHeight, kSlices, kSrcZ, kDstZ, kDepth, "2d_array->3d");
        }

        // 3d -> 2d_array, the mirror image: the depth now has to reach the DESTINATION's layerCount
        // while the source states it as extent.depth from a z offset.
        TEST_F(CopyImageLayeredScenario, VolumeToArrayCopiesEverySlice) {
            if (!Ready() || IsSkipped()) return;

            const GLuint source = MakeTexture(GL_TEXTURE_3D, 1, SourceColor);
            const GLuint destination = MakeTexture(GL_TEXTURE_2D_ARRAY, 1, DestinationFill);
            ASSERT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR)) << "texture setup failed";

            constexpr int kSrcZ = 1;
            constexpr int kDstZ = 2;
            constexpr int kDepth = 4;
            glCopyImageSubData(source, GL_TEXTURE_3D, 0, 0, 0, kSrcZ, destination, GL_TEXTURE_2D_ARRAY, 0, 0, 0, kDstZ,
                               kWidth, kHeight, kDepth);
            ASSERT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR)) << "glCopyImageSubData raised an error";

            ExpectCopied(destination, 0, kWidth, kHeight, kSlices, kSrcZ, kDstZ, kDepth, "3d->2d_array");
        }

    } // namespace
} // namespace MGITest
