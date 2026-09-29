// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/DescriptorPoolGrowthScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - ONE FRAME THAT NEEDS THOUSANDS OF DISTINCT DESCRIPTOR SETS.
//
// DirectVulkan takes a draw's descriptor set from a per-frame-slot cache and adds a pool to the
// slot when the cache runs dry (UniformManager). Two things went wrong with a burst like
// Minecraft rd12's loading frame (15,016 sets in one slot):
//   - Growth was linear. The next pool was sized at twice the slot's FIRST pool of the flavour,
//     which is always its oldest and smallest, so every grown pool came out the same size: the
//     loading slot ended with 59 pools of 256 sets.
//   - The burst was kept forever. The cache only rewinds, so the slot held all 15k sets (and the
//     pools behind them) for the life of the process, while steady frames used a few dozen.
// Now each pool doubles the slot's largest one (capped), and a slot whose pools stay under a
// quarter full for MOBILEGL_MAGMA_DESCRIPTOR_TRIM_FRAMES frames is handed back whole and re-grows.
//
// Each of kDraws draws paints its own pixel of a 64x64 target and samples one of kTextures
// textures, a different one from each of the previous draws' so neither the renderer's per-draw
// descriptor reuse memo (four entries) nor its dynamic-offset-only rebind can let two draws share
// a set: one frame needs kDraws sets. Every pixel must hold its own draw's texture colour, on both
// backends. On DirectVulkan the cases also read the pools through GetDescriptorPoolCensus - which
// only the desktop build can reach, since the integration harness links the static library there
// and the hidden-visibility shipping library on Android.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#include "../Harness/HeadlessGL.h"
#include "../Harness/ScenarioFixture.h"
#include "../Harness/SplitRuntimePeek.h"

#if !defined(__ANDROID__)
#include <MG_Backend/DirectVulkan/DescriptorPoolCensus.h>
#endif

#ifdef GLAPI
#undef GLAPI
#endif
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glcorearb.h>
#undef GL_GLEXT_PROTOTYPES

namespace MGITest {
    namespace {

        constexpr int kSide = 64;
        constexpr int kDraws = kSide * kSide; // well under the 16384-draw command-buffer split budget
        // More than the descriptor reuse memo's four entries, so the texture a draw samples is never
        // one of the last four draws'.
        constexpr int kTextures = 8;
        // The base pool size (VulkanRenderer's kDescriptorSetsPerFrame) and the growth cap
        // (UniformManager::kMaxDescriptorPoolSets).
        constexpr std::uint64_t kBaseSets = 64;
        constexpr std::uint64_t kMaxPoolSets = 4096;

        constexpr const char* kVS = R"(#version 330 core
in vec2 aPos;
uniform int uIndex;
void main() {
    vec2 cell = vec2(float(uIndex % 64), float(uIndex / 64));
    gl_Position = vec4((cell + aPos) / 32.0 - 1.0, 0.0, 1.0);
}
)";
        constexpr const char* kFS = R"(#version 330 core
uniform sampler2D uTex;
out vec4 o_color;
void main() { o_color = texture(uTex, vec2(0.5)); }
)";

        // Texture t's single texel; every channel differs between any two textures.
        Rgba8 TextureColor(int t) {
            return Rgba8{static_cast<std::uint8_t>(16 + 32 * t), static_cast<std::uint8_t>(240 - 32 * t),
                         static_cast<std::uint8_t>(8 + 29 * t), 255};
        }

        std::string Describe(const Rgba8& c) {
            std::ostringstream os;
            os << "rgba(" << static_cast<int>(c.r) << ", " << static_cast<int>(c.g) << ", " << static_cast<int>(c.b)
               << ", " << static_cast<int>(c.a) << ")";
            return os.str();
        }

        unsigned long EnvUnsigned(const char* name, unsigned long fallback) {
            const char* value = std::getenv(name);
            if (value == nullptr || *value == '\0') return fallback;
            char* end = nullptr;
            const unsigned long parsed = std::strtoul(value, &end, 10);
            return (end != nullptr && *end == '\0') ? parsed : fallback;
        }

        // P11 M2: HOW MANY OF THE BURST'S SETS ONE FRAME SLOT HOLDS AT ONCE. All kDraws of them on
        // the monolith arm. Under a split transport the server draws through the WIRE arm, which
        // rewinds a slot's set cursors every UniformManager::kWireDescriptorSetBudget (2048) sets once
        // it has proved the GPU done with them (VulkanRenderer::RewindWireDescriptorSetsIfDue), so its
        // cache holds the burst 2048 sets at a time: the burst still needs a set per draw, grows the
        // slot geometrically to that and is given back by the same trim, only the census counts the
        // sets held rather than the draws. (The census itself is in-process - inproc it is read on the
        // server's apply thread; spawn and tcp answer unavailable - see GetDescriptorPoolCensus.)
        constexpr std::uint64_t kWireDescriptorSetBudget = 2048;
        std::uint64_t BurstSetsHeldAtOnce() {
            const std::uint64_t draws = static_cast<std::uint64_t>(kDraws);
            return PeekSplitRuntime().transportResolved ? std::min(draws, kWireDescriptorSetBudget) : draws;
        }

        // Pools a geometric policy adds for n sets from an empty flavour: 64, 128, ... up to the cap,
        // then one per cap. The old first-pool rule needed n/128 or n/256 of them.
        std::uint64_t GeometricPoolBound(std::uint64_t n) {
            std::uint64_t pools = 0, capacity = 0, size = kBaseSets;
            while (capacity < n) {
                capacity += size;
                ++pools;
                size = std::min(size * 2, kMaxPoolSets);
            }
            return pools;
        }

        struct Census {
            bool available = false;
            std::uint64_t pools = 0;
            std::uint64_t capacitySets = 0;
            std::uint64_t cachedSets = 0;
            std::uint64_t slotTrims = 0;
        };

        Census ReadCensus() {
            Census out;
#if !defined(__ANDROID__)
            const auto census = MobileGL::MG_Backend::DirectVulkan::GetDescriptorPoolCensus();
            out.available = census.available;
            out.pools = census.pools;
            out.capacitySets = census.capacitySets;
            out.cachedSets = census.cachedSets;
            out.slotTrims = census.slotTrims;
#endif
            return out;
        }

        std::string Describe(const Census& c) {
            std::ostringstream os;
            os << "pools=" << c.pools << " capacity=" << c.capacitySets << " cachedSets=" << c.cachedSets
               << " slotTrims=" << c.slotTrims;
            return os.str();
        }

        ::testing::AssertionResult EveryPixelHoldsItsDrawsTexture(const Image& image, const char* when) {
            int wrong = 0;
            int firstX = -1, firstY = -1;
            Rgba8 firstSeen{};
            for (int y = 0; y < kSide; ++y) {
                for (int x = 0; x < kSide; ++x) {
                    const Rgba8 expected = TextureColor((y * kSide + x) % kTextures);
                    const Rgba8 seen = image.At(x, y);
                    if (seen != expected) {
                        if (wrong == 0) {
                            firstX = x;
                            firstY = y;
                            firstSeen = seen;
                        }
                        ++wrong;
                    }
                }
            }
            if (wrong == 0) return ::testing::AssertionSuccess();
            const int draw = firstY * kSide + firstX;
            return ::testing::AssertionFailure()
                   << when << ": " << wrong << " of " << kDraws << " pixels do not hold their own draw's texture. "
                   << "First: (" << firstX << ", " << firstY << ") = " << Describe(firstSeen) << ", drawn by draw "
                   << draw << " with texture " << (draw % kTextures) << " = "
                   << Describe(TextureColor(draw % kTextures)) << " (a pixel no draw reached holds 0,0,0,0)";
        }

        class DescriptorPoolGrowthScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                static const float kVertices[] = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f};
                glGenVertexArrays(1, &m_vao);
                glBindVertexArray(m_vao);
                glGenBuffers(1, &m_vbo);
                glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
                glBufferData(GL_ARRAY_BUFFER, sizeof(kVertices), kVertices, GL_STATIC_DRAW);
                glEnableVertexAttribArray(0);
                glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);

                m_textures.resize(kTextures);
                glGenTextures(kTextures, m_textures.data());
                for (int t = 0; t < kTextures; ++t) {
                    const Rgba8 c = TextureColor(t);
                    const std::uint8_t texel[4] = {c.r, c.g, c.b, c.a};
                    glBindTexture(GL_TEXTURE_2D, m_textures[t]);
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, texel);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                }
                glBindTexture(GL_TEXTURE_2D, 0);
            }

            void TearDown() override {
                if (Ready()) {
                    glUseProgram(0);
                    glBindVertexArray(0);
                    glBindTexture(GL_TEXTURE_2D, 0);
                    BindDefaultFramebuffer();
                    if (m_program != 0) glDeleteProgram(m_program);
                    if (!m_textures.empty()) glDeleteTextures(static_cast<GLsizei>(m_textures.size()), m_textures.data());
                    if (m_vbo != 0) glDeleteBuffers(1, &m_vbo);
                    if (m_vao != 0) glDeleteVertexArrays(1, &m_vao);
                    DestroyColorFbo(m_target);
                }
                ScenarioTest::TearDown();
            }

            ::testing::AssertionResult Prepare() {
                std::string error;
                m_program = CompileProgram(kVS, kFS, &error);
                if (m_program == 0) return ::testing::AssertionFailure() << error;
                m_indexLocation = glGetUniformLocation(m_program, "uIndex");
                m_target = MakeColorFbo(kSide, kSide);
                if (m_target.fbo == 0) return ::testing::AssertionFailure() << "MakeColorFbo failed";
                glUseProgram(m_program);
                glUniform1i(glGetUniformLocation(m_program, "uTex"), 0);
                glActiveTexture(GL_TEXTURE0);
                glBindVertexArray(m_vao);
                BindFbo(m_target);
                glDisable(GL_SCISSOR_TEST);
                glDisable(GL_DEPTH_TEST);
                glDisable(GL_BLEND);
                return ::testing::AssertionSuccess();
            }

            // Draw i paints pixel (i % 64, i / 64) with texture i % kTextures.
            void Draw(int i) {
                glBindTexture(GL_TEXTURE_2D, m_textures[i % kTextures]);
                glUniform1i(m_indexLocation, i);
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            }

            // The burst: every pixel, one draw and one descriptor set each.
            void DrawBurst() {
                ClearTo(0.0f, 0.0f, 0.0f, 0.0f);
                for (int i = 0; i < kDraws; ++i) Draw(i);
            }

            static bool IsVulkan() { return Gl().BackendName() == std::string("DirectVulkan"); }

            GLuint m_vao = 0;
            GLuint m_vbo = 0;
            GLuint m_program = 0;
            GLint m_indexLocation = -1;
            std::vector<GLuint> m_textures;
            ColorFbo m_target;
        };

    } // namespace

    // One frame, kDraws distinct sets. Every pixel lands, and the frame slot grows O(log N) pools
    // for it, not N / 128.
    TEST_F(DescriptorPoolGrowthScenario, AFrameOfDistinctSetsLandsWholeAndGrowsItsPoolsGeometrically) {
        if (!Ready()) return;
        ASSERT_TRUE(Prepare());

        const Census before = ReadCensus();
        DrawBurst();
        const Image image = ReadPixels(kSide, kSide);
        const Census after = ReadCensus();

        EXPECT_TRUE(EveryPixelHoldsItsDrawsTexture(image, "one frame of distinct descriptor sets"));
        EXPECT_EQ(FirstGLError(), 0u);

        RecordProperty("census_checked", (IsVulkan() && after.available) ? "yes" : "no");
        if (!IsVulkan() || !after.available) return;
        // Falsifiability first: had the renderer's memos collapsed the draws onto shared sets, the
        // slot would never have needed to grow and the bound below would pass on any policy.
        const std::uint64_t held = BurstSetsHeldAtOnce();
        ASSERT_GE(after.cachedSets, before.cachedSets + held)
            << "the frame did not allocate one descriptor set per draw, so it does not exercise growth. Before: "
            << Describe(before) << "; after: " << Describe(after);
        const std::uint64_t added = after.pools - before.pools;
        RecordProperty("census_before", Describe(before));
        RecordProperty("census_after", Describe(after));
        EXPECT_LE(added, GeometricPoolBound(held + 1))
            << "a frame needing " << held << " descriptor sets at once added " << added << " pools; doubling the "
            << "slot's largest pool needs at most " << GeometricPoolBound(held + 1) << ". Before: " << Describe(before)
            << "; after: " << Describe(after);
    }

    // Burst, then quiet frames until the renderer hands the burst back, then the burst again while
    // the quiet frames are still in flight. Nothing waits between frames except where noted, so the
    // trim runs with other frame slots' work outstanding.
    TEST_F(DescriptorPoolGrowthScenario, AGivenBackBurstLeavesNoStaleSetBehind) {
        if (!Ready()) return;
        ASSERT_TRUE(Prepare());
        const unsigned long trimFrames = EnvUnsigned("MOBILEGL_MAGMA_DESCRIPTOR_TRIM_FRAMES", 120);
        const unsigned long framesInFlight = EnvUnsigned("MOBILEGL_MAGMA_FRAMESINFLIGHT", 3);
        // The trim fires once trimFrames quiet frames have followed the burst, at the burst slot's next
        // turn; past this many frames it is not coming.
        const unsigned long quietFrameLimit = trimFrames + 2 * framesInFlight + 16;

        DrawBurst();
        Gl().EndFrame();
        const Census afterBurst = ReadCensus();

        // Quiet frames: one draw each, the same pixel and texture the burst gave draw 0.
        const bool observe = IsVulkan() && afterBurst.available && trimFrames != 0;
        unsigned long quietFrames = 0;
        Census afterQuiet = afterBurst;
        for (; quietFrames < quietFrameLimit; ++quietFrames) {
            Draw(0);
            Gl().EndFrame();
            if (observe) {
                afterQuiet = ReadCensus();
                if (afterQuiet.slotTrims > afterBurst.slotTrims) {
                    ++quietFrames;
                    break;
                }
            }
        }

        // The burst again, straight away. The slot it lands in re-grows from its base pool.
        DrawBurst();
        const Image image = ReadPixels(kSide, kSide);
        const Census afterSecondBurst = ReadCensus();

        EXPECT_TRUE(EveryPixelHoldsItsDrawsTexture(image, "the burst drawn after the first one was given back"));
        EXPECT_EQ(FirstGLError(), 0u);

        RecordProperty("census_checked", observe ? "yes" : "no");
        if (!observe) return;
        RecordProperty("census_after_burst", Describe(afterBurst));
        RecordProperty("quiet_frames", std::to_string(quietFrames));
        RecordProperty("census_after_quiet", Describe(afterQuiet));
        RecordProperty("census_after_second_burst", Describe(afterSecondBurst));
        const std::uint64_t held = BurstSetsHeldAtOnce();
        ASSERT_GE(afterBurst.cachedSets, held)
            << "the burst did not allocate one descriptor set per draw: " << Describe(afterBurst);
        EXPECT_GT(afterQuiet.slotTrims, afterBurst.slotTrims)
            << quietFrames << " frames of one draw each followed a " << kDraws << "-set frame, but no frame slot was "
            << "trimmed (MOBILEGL_MAGMA_DESCRIPTOR_TRIM_FRAMES=" << trimFrames << "). After the burst: "
            << Describe(afterBurst) << "; after the quiet frames: " << Describe(afterQuiet);
        EXPECT_LE(afterQuiet.cachedSets + held / 2, afterBurst.cachedSets)
            << "the trim did not give the burst's sets back. After the burst: " << Describe(afterBurst)
            << "; after the quiet frames: " << Describe(afterQuiet);
        EXPECT_LT(afterQuiet.pools, afterBurst.pools)
            << "the trim did not give the burst's pools back. After the burst: " << Describe(afterBurst)
            << "; after the quiet frames: " << Describe(afterQuiet);
        EXPECT_GE(afterSecondBurst.cachedSets, afterQuiet.cachedSets + held)
            << "the second burst was not served by freshly allocated sets: " << Describe(afterSecondBurst);
    }

} // namespace MGITest
