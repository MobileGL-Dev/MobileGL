// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/WireMipShapeScenario.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// P8-SV: TWO glGenerateMipmap SHAPES THE MAGMA WIRE ARM GOT WRONG (docs/Disaggregated/notes/p8/SV.md).
//
//   * A 3D chain through the NATIVE blit. The wire arm passed the level's depth as the blit's
//     subresource layerCount on an image with one array layer (VUID-vkCmdBlitImage-srcImage-00240,
//     -srcSubresource-01707, -dstSubresource-01708). Lavapipe blits a 3D image by its z offsets and
//     ignores layerCount, so the PIXELS below are right with or without the fix on this host; what
//     goes red on the host is the validation layer, which the `.Mip3DValidation.` entry runs this
//     case under (inproc, so the server's Vulkan instance is this process and its stdout is ours).
//     The pixel half runs on both backends and every arm, and is what a driver that honours
//     layerCount would fail.
//   * A 1D DEPTH chain with no native blit. The depth shader arm renders 2D views, which a 1D image
//     does not admit; the wire arm handed it the chain anyway. It is now the named decline
//     MipmapShaderFormatOrShape, decided before the chain grows, and the session carries on.
//     Magma only, and reachable on this host only with MGITEST_MAGMA_FORCE_SHADER_MIPMAP (lavapipe
//     blits depth natively), which the SERVER reads: split and spawn entries only.

#include <array>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "../Harness/HeadlessGL.h"
#include "../Harness/ScenarioFixture.h"
#include "../Harness/SplitLane.h"
#include "../Harness/SplitRuntimePeek.h"
#include "../Harness/WireDeclinePeek.h"

#ifdef GLAPI
#undef GLAPI
#endif
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>

namespace MGITest {
    namespace {

        using Texel = std::array<int, 4>;

        // Is the Khronos validation layer loaded into THIS process? On the inproc arm the server's
        // Vulkan instance lives here, so this is the proof that a run "under the layer" really was.
        bool ValidationLayerMapped() {
#if defined(__linux__)
            std::ifstream maps("/proc/self/maps");
            std::string line;
            while (std::getline(maps, line)) {
                if (line.find("VkLayer_khronos_validation") != std::string::npos) return true;
            }
#endif
            return false;
        }

        class WireMipShapeScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                m_magma = Gl().BackendName() == "DirectVulkan";
                m_transport = PeekSplitRuntime().transportName;
            }

            // The Magma server's decline tally is readable where the server is this process.
            bool DeclinesReadable() const { return m_magma && m_transport == "inproc"; }

            bool m_magma = false;
            std::string m_transport;
        };

        // A 3D RGBA8 chain, 8x8x8 with its full storage, whose level-0 slices come in PAIRS of one
        // colour: level 1's slice k is then pair k's colour exactly, and every level below is the
        // average of two slices of the level above. A blit that loses the z axis (one slice only,
        // or slices read as array layers of a 2D image) cannot produce those numbers.
        TEST_F(WireMipShapeScenario, VolumeChainIsGeneratedAlongItsDepth) {
            if (!Ready() || IsSkipped()) return;
            constexpr int kSide = 8, kLevels = 4;
            const std::array<Texel, 4> pairs = {Texel{200, 40, 80, 255}, Texel{40, 200, 120, 255},
                                                Texel{100, 100, 240, 255}, Texel{240, 160, 0, 255}};
            GLuint volume = 0;
            glGenTextures(1, &volume);
            glBindTexture(GL_TEXTURE_3D, volume);
            glTexStorage3D(GL_TEXTURE_3D, kLevels, GL_RGBA8, kSide, kSide, kSide);
            std::vector<GLubyte> level0(static_cast<size_t>(kSide * kSide * kSide * 4));
            for (int z = 0; z < kSide; ++z) {
                const Texel& texel = pairs[static_cast<size_t>(z / 2)];
                for (int at = 0; at < kSide * kSide; ++at) {
                    for (int c = 0; c < 4; ++c)
                        level0[static_cast<size_t>((z * kSide * kSide + at) * 4 + c)] = static_cast<GLubyte>(texel[c]);
                }
            }
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            glTexSubImage3D(GL_TEXTURE_3D, 0, 0, 0, 0, kSide, kSide, kSide, GL_RGBA, GL_UNSIGNED_BYTE, level0.data());
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "3D texture setup";
            glGenerateMipmap(GL_TEXTURE_3D);
            ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "glGenerateMipmap(GL_TEXTURE_3D)";

            const auto average = [](const Texel& a, const Texel& b) {
                return Texel{(a[0] + b[0]) / 2, (a[1] + b[1]) / 2, (a[2] + b[2]) / 2, (a[3] + b[3]) / 2};
            };
            // expected[level][slice]
            std::vector<std::vector<Texel>> expected(kLevels);
            expected[1].assign(pairs.begin(), pairs.end());
            expected[2] = {average(pairs[0], pairs[1]), average(pairs[2], pairs[3])};
            expected[3] = {average(expected[2][0], expected[2][1])};
            glPixelStorei(GL_PACK_ALIGNMENT, 4);
            for (int level = 1; level < kLevels; ++level) {
                const int side = kSide >> level;
                GLint width = 0, height = 0, depth = 0;
                glGetTexLevelParameteriv(GL_TEXTURE_3D, level, GL_TEXTURE_WIDTH, &width);
                glGetTexLevelParameteriv(GL_TEXTURE_3D, level, GL_TEXTURE_HEIGHT, &height);
                glGetTexLevelParameteriv(GL_TEXTURE_3D, level, GL_TEXTURE_DEPTH, &depth);
                EXPECT_EQ(width, side) << "level " << level;
                EXPECT_EQ(height, side) << "level " << level;
                EXPECT_EQ(depth, side) << "level " << level;
                std::vector<GLubyte> pixels(static_cast<size_t>(side * side * side * 4), 0x5a);
                glGetTexImage(GL_TEXTURE_3D, level, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
                ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "readback of level " << level;
                for (int z = 0; z < side; ++z) {
                    const Texel& want = expected[static_cast<size_t>(level)][static_cast<size_t>(z)];
                    size_t wrong = 0;
                    Texel first{};
                    for (int at = 0; at < side * side; ++at) {
                        const size_t base = static_cast<size_t>((z * side * side + at) * 4);
                        bool ok = true;
                        for (int c = 0; c < 4; ++c) {
                            const int got = pixels[base + static_cast<size_t>(c)];
                            if (got < want[c] - 2 || got > want[c] + 2) ok = false;
                        }
                        if (!ok && wrong++ == 0) {
                            for (int c = 0; c < 4; ++c) first[c] = pixels[base + static_cast<size_t>(c)];
                        }
                    }
                    EXPECT_EQ(wrong, 0u) << "level " << level << " slice " << z << ": expected (" << want[0] << ","
                                         << want[1] << "," << want[2] << "," << want[3] << "), first wrong texel ("
                                         << first[0] << "," << first[1] << "," << first[2] << "," << first[3] << ")";
                }
            }
            glBindTexture(GL_TEXTURE_3D, 0);
            glDeleteTextures(1, &volume);

            // The `.Mip3DValidation.` entry: the blit above ran under the Khronos validation layer,
            // whose VUID lines the entry's FAIL_REGULAR_EXPRESSION reads. Without the layer actually
            // loaded, a clean output would prove nothing, so the entry requires it.
            if (SplitLane::MarkerIsOne("MGITEST_REQUIRE_VK_VALIDATION_LAYER")) {
                ASSERT_TRUE(m_magma && m_transport == "inproc")
                    << "the validation entry reads the server's layer output in-process: Magma, inproc";
                EXPECT_TRUE(ValidationLayerMapped())
                    << "VK_LAYER_KHRONOS_validation is not loaded in this process, so the absence of "
                       "VUID lines in this entry's output says nothing";
            }
        }

        // A 1D DEPTH chain with the native blit forced off: no mip arm renders a 1D image, so the
        // wire arm declines by name (MipmapShaderFormatOrShape) before growing the chain. Pinned:
        // no GL error, the session answering (two readback round trips), level 0 as uploaded, level
        // 1 still the application's own sentinel - and, where the server is this process, exactly
        // one MipmapShaderFormatOrShape decline counted.
        TEST_F(WireMipShapeScenario, Depth1DChainWithoutANativeBlitDeclinesByName) {
            if (!Ready() || IsSkipped()) return;
            if (!m_magma)
                GTEST_SKIP() << "Magma wire-arm decline (P8-SV): the knob and the depth shader arm it forces are "
                                "Magma's; Espryt (" << Gl().BackendName() << ") generates this chain itself";
            const std::string tier = SplitLane::MarkerValue("MGITEST_MAGMA_FORCE_SHADER_MIPMAP");
            if (tier.empty())
                GTEST_SKIP() << "MGITEST_MAGMA_FORCE_SHADER_MIPMAP is unset: this driver blits depth natively, "
                                "so the depth shader arm this case refuses is not entered";
            constexpr int kWidth = 16;
            GLuint line = 0;
            glGenTextures(1, &line);
            glBindTexture(GL_TEXTURE_1D, line);
            glTexStorage1D(GL_TEXTURE_1D, 5, GL_DEPTH_COMPONENT32F, kWidth);
            const std::vector<GLfloat> level0(kWidth, 0.25f), level1(kWidth / 2, 0.75f);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            glTexSubImage1D(GL_TEXTURE_1D, 0, 0, kWidth, GL_DEPTH_COMPONENT, GL_FLOAT, level0.data());
            glTexSubImage1D(GL_TEXTURE_1D, 1, 0, kWidth / 2, GL_DEPTH_COMPONENT, GL_FLOAT, level1.data());
            ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "1D depth texture setup";
            unsigned long long declinesBefore = 0;
            const bool counted =
                DeclinesReadable() && PeekWireDeclineCount("MipmapShaderFormatOrShape", &declinesBefore);
            glGenerateMipmap(GL_TEXTURE_1D);
            EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "glGenerateMipmap(GL_TEXTURE_1D) on a depth chain";
            glPixelStorei(GL_PACK_ALIGNMENT, 4);
            for (const int level : {0, 1}) {
                const int width = kWidth >> level;
                std::vector<GLfloat> depths(static_cast<size_t>(width), -1.0f);
                glGetTexImage(GL_TEXTURE_1D, level, GL_DEPTH_COMPONENT, GL_FLOAT, depths.data());
                ASSERT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << "readback of level " << level;
                const GLfloat want = level == 0 ? 0.25f : 0.75f;
                size_t wrong = 0;
                GLfloat first = 0.0f;
                for (const GLfloat depth : depths) {
                    if (depth < want - 0.0001f || depth > want + 0.0001f) {
                        if (wrong++ == 0) first = depth;
                    }
                }
                EXPECT_EQ(wrong, 0u) << "level " << level << " tier=" << tier << ": a declined mip must leave "
                                     << "every level as the application wrote it (expected " << want
                                     << ", first wrong " << first << ")";
            }
            glBindTexture(GL_TEXTURE_1D, 0);
            glDeleteTextures(1, &line);
            if (counted) {
                unsigned long long declinesAfter = 0;
                ASSERT_TRUE(PeekWireDeclineCount("MipmapShaderFormatOrShape", &declinesAfter));
                EXPECT_EQ(declinesAfter - declinesBefore, 1u)
                    << "the 1D depth chain must be refused once, by name, before the chain grows";
            }
        }

    } // namespace
} // namespace MGITest
