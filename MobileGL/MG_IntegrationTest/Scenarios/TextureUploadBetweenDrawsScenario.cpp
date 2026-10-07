// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/TextureUploadBetweenDrawsScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - NEW TEXELS FOR A TEXTURE BETWEEN TWO DRAWS OF ONE FRAME THAT SAMPLE IT.
//
// Draw A samples texture T, the application uploads new texels to T, draw B samples it again, and
// nothing in between waits: no glFinish, no readback, no fence. A must see the old texels and B the
// new ones. Magma stages the upload into a batch it submits on the graphics queue AFTER flushing
// the recording that holds A, and orders the copy behind A's reads with the batch's first barrier
// (source scope from the image's tracked layout) rather than with a CPU wait for the GPU to drain.
// The pixels say whether the order held on this device; the `.SyncValidation.` entries run the
// same cases under the Khronos synchronization validator, whose SYNC-HAZARD lines red the entry
// whatever the device's timing, so a barrier that stops covering A's reads goes red there even
// where the GPU happens to finish A first.
//
// The second case takes the PRESERVE path in between: defining a new mip level re-mints T's image
// and copies the old image's texels into the new one while A may still be reading the old one.

#include <array>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "../Harness/HeadlessGL.h"
#include "../Harness/ScenarioFixture.h"
#include "../Harness/SplitLane.h"

#ifdef GLAPI
#undef GLAPI
#endif
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glcorearb.h>
#undef GL_GLEXT_PROTOTYPES

namespace MGITest {
    namespace {
        constexpr int kTile = 8;
        constexpr int kTiles = 4;

        constexpr const char* kVS = R"(#version 430 core
void main() {
    vec2 positions[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    gl_Position = vec4(positions[gl_VertexID], 0.0, 1.0);
}
)";
        // texelFetch at a uniform lod: no filtering, no completeness question for the level read.
        // The loop keeps each draw on the GPU for a while, so the upload after it is submitted while
        // the draw is still in flight: on a real GPU a missing dependency then shows in the pixels,
        // and under the synchronization validator the earlier submission has not been seen to
        // complete (a fence poll that already saw it done would retire its accesses). The loop's
        // result only matters in a branch no input reaches, which keeps it from being folded away.
        constexpr const char* kFS = R"(#version 430 core
uniform sampler2D u_tex;
uniform int u_lod;
uniform int u_spin;
out vec4 o_color;
void main() {
    vec4 texel = texelFetch(u_tex, ivec2(0, 0), u_lod);
    float acc = texel.r;
    for (int i = 0; i < u_spin; ++i) acc = fract(acc * 1.0001 + 0.37);
    o_color = acc == -1.0 ? vec4(1.0, 0.0, 1.0, 1.0) : texel;
}
)";
        constexpr int kSpin = 200000;

        using Rgba = std::array<std::uint8_t, 4>;
        constexpr Rgba kRed{255, 0, 0, 255};
        constexpr Rgba kGreen{0, 255, 0, 255};
        constexpr Rgba kBlue{0, 0, 255, 255};
        constexpr Rgba kYellow{255, 255, 0, 255};

        std::vector<std::uint8_t> Solid(int texels, const Rgba& c) {
            std::vector<std::uint8_t> out(static_cast<std::size_t>(texels) * 4u);
            for (int i = 0; i < texels; ++i)
                for (int k = 0; k < 4; ++k) out[static_cast<std::size_t>(i) * 4u + k] = c[k];
            return out;
        }

        class TextureUploadBetweenDrawsScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                m_target = MakeColorFbo(kTile * kTiles, kTile);
                ASSERT_NE(m_target.fbo, 0u);
                std::string error;
                m_program = CompileProgram(kVS, kFS, &error);
                ASSERT_NE(m_program, 0u) << error;
                glGenVertexArrays(1, &m_vao);
                glGenTextures(1, &m_texture);
                BindFbo(m_target);
                glDisable(GL_SCISSOR_TEST);
                glDisable(GL_DEPTH_TEST);
                glDisable(GL_BLEND);
                ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
                glFinish();
                FirstGLError();
            }

            void TearDown() override {
                if (Ready()) {
                    glUseProgram(0);
                    if (m_program) glDeleteProgram(m_program);
                    if (m_texture) glDeleteTextures(1, &m_texture);
                    if (m_vao) glDeleteVertexArrays(1, &m_vao);
                    BindDefaultFramebuffer();
                    DestroyColorFbo(m_target);
                    glViewport(0, 0, Gl().Width(), Gl().Height());
                    FirstGLError();
                }
            }

            // One draw into tile `tile` that samples level `lod` of the texture. No wait after it.
            void DrawTile(int tile, int lod) {
                BindFbo(m_target);
                glViewport(tile * kTile, 0, kTile, kTile);
                glUseProgram(m_program);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, m_texture);
                glUniform1i(glGetUniformLocation(m_program, "u_tex"), 0);
                glUniform1i(glGetUniformLocation(m_program, "u_lod"), lod);
                glUniform1i(glGetUniformLocation(m_program, "u_spin"), kSpin);
                glBindVertexArray(m_vao);
                glDrawArrays(GL_TRIANGLES, 0, 3);
                glBindVertexArray(0);
            }

            void ExpectTile(int tile, const Rgba& want, const char* why) {
                BindFbo(m_target);
                std::array<std::uint8_t, 4> got{};
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glReadPixels(tile * kTile + kTile / 2, kTile / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, got.data());
                EXPECT_EQ(got, want) << why << " (tile " << tile << ")";
            }

            void DefineLevel0(int edge, const Rgba& c, int maxLevel) {
                glBindTexture(GL_TEXTURE_2D, m_texture);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, maxLevel);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                const auto bytes = Solid(edge * edge, c);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, edge, edge, 0, GL_RGBA, GL_UNSIGNED_BYTE, bytes.data());
            }

            void SubLevel(int level, int edge, const Rgba& c) {
                glBindTexture(GL_TEXTURE_2D, m_texture);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                const auto bytes = Solid(edge * edge, c);
                glTexSubImage2D(GL_TEXTURE_2D, level, 0, 0, edge, edge, GL_RGBA, GL_UNSIGNED_BYTE, bytes.data());
            }

            // The `.SyncValidation.` entries: a clean output proves nothing unless the layer loaded.
            void ExpectValidationLayerWhenRequired() {
                if (!SplitLane::MarkerIsOne("MGITEST_REQUIRE_VK_VALIDATION_LAYER")) return;
#if defined(__linux__)
                std::ifstream maps("/proc/self/maps");
                std::string line;
                bool mapped = false;
                while (std::getline(maps, line)) {
                    if (line.find("VkLayer_khronos_validation") != std::string::npos) mapped = true;
                }
                EXPECT_TRUE(mapped) << "VK_LAYER_KHRONOS_validation is not loaded in this process, so the absence "
                                       "of SYNC-HAZARD lines in this entry's output says nothing";
#endif
            }

            ColorFbo m_target;
            GLuint m_program = 0;
            GLuint m_vao = 0;
            GLuint m_texture = 0;
        };

        // A samples red, a sub-image upload makes it green, B samples green; then once more with blue
        // and yellow, so the second upload also lands behind a draw of the batch the first one opened.
        TEST_F(TextureUploadBetweenDrawsScenario, EachDrawSeesTheTexelsUploadedBeforeIt) {
            if (!Ready() || IsSkipped()) return;
            DefineLevel0(2, kRed, 0);
            DrawTile(0, 0);
            SubLevel(0, 2, kGreen);
            DrawTile(1, 0);
            SubLevel(0, 2, kBlue);
            DrawTile(2, 0);
            SubLevel(0, 2, kYellow);
            DrawTile(3, 0);
            EXPECT_EQ(FirstGLError(), 0u);
            ExpectTile(0, kRed, "draw issued before the first upload");
            ExpectTile(1, kGreen, "draw issued after the first upload");
            ExpectTile(2, kBlue, "draw issued after the second upload");
            ExpectTile(3, kYellow, "draw issued after the third upload");
            ExpectValidationLayerWhenRequired();
        }

        // The re-mint between the draws: level 1 did not exist when A sampled level 0, so defining
        // it grows the chain - a new image, with the old one's texels copied across (preserve) -
        // and the sub-image upload after it writes the new image.
        TEST_F(TextureUploadBetweenDrawsScenario, AChainGrowthBetweenTheDrawsPreservesWhatTheFirstOneSampled) {
            if (!Ready() || IsSkipped()) return;
            DefineLevel0(2, kRed, 1);
            DrawTile(0, 0);
            {
                const auto level1 = Solid(1, kGreen);
                glBindTexture(GL_TEXTURE_2D, m_texture);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                glTexImage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, level1.data());
            }
            DrawTile(1, 0);
            DrawTile(2, 1);
            SubLevel(0, 2, kBlue);
            DrawTile(3, 0);
            EXPECT_EQ(FirstGLError(), 0u);
            ExpectTile(0, kRed, "draw issued before the chain grew");
            ExpectTile(1, kRed, "level 0 after the chain grew (the preserved texels)");
            ExpectTile(2, kGreen, "the new level 1");
            ExpectTile(3, kBlue, "level 0 after the upload into the re-minted image");
            ExpectValidationLayerWhenRequired();
        }
    } // namespace
} // namespace MGITest
