// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/GenerateMipmapServerScenario.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - glGenerateMipmap ON THE FORMATS ESPRYT DOES NOT HAND STRAIGHT TO THE DRIVER
// (P8-B, docs/Disaggregated/notes/p8/B.md).
//
// Espryt generates R11F_G11F_B10F and depth chains with per-level blits, and RGB16F / RGB32F
// chains outside glGenerateMipmap because ES refuses a three-channel float format there. Until
// P8-B the split arms of both were named refusals:
//   B1  Fatal{UnmigratedEmulation, "generate-mipmap-storage"} whenever the texture's descriptor
//       held fewer levels than the full chain from level 0 - a MAX_LEVEL below the chain, or a
//       glTexStorage2D with fewer levels - which is legal GL (the generated window is
//       BASE_LEVEL+1 .. min(p, MAX_LEVEL), and immutable storage bounds it too);
//   B2  Fatal{UnmigratedEmulation, "generate-mipmap-cpu-filter"} on every RGB16F / RGB32F
//       generation, because the monolith filters the frontend's level shadows on the CPU.
//
// EVERY CASE CHECKS THE GL ANSWER BY SAMPLING. The base level is split into two halves (bottom
// rows A, top rows B) so a generation that loses the row order, or reads another level, cannot
// average to the right colour. Each generated level is read back through texelFetch into an RGBA8
// target, and the levels outside the window are checked unchanged (or still undefined).
//
// ARMS. Both backends, monolith and Split / Spawn / Tcp; Magma is the control. The forced-CPU arm
// (MGITEST_ESPRYT_FORCE_CPU_MIPMAP=1, read by the SERVER) runs the three-channel cases through
// Espryt's server-side CPU filter, which a host driver never needs: llvmpipe generates both
// formats natively. Under the knob the server log must name that arm, or the entry proved nothing.
//
// THE CPU ARM'S STORE ROUTE (P8-SE). When the driver cannot read the base level back, the server's
// CPU arm filters the staged store's copy of it - but only for a texture the driver never wrote
// (P8-E's sticky mark), because a render target's level 0 exists only on the driver and the store
// still holds what crossed before the draw. The `CpuMipRefused.` entries add
// MGITEST_ESPRYT_REFUSE_TEXTURE_READBACK_EXTENT=16x16 (P8-E's knob, also read by the SERVER) to the
// forced-CPU knob: a render-target base must decline by name (generate-mipmap-store-declined) and
// never filter the stale store, and an uploaded-only base must still be filtered from the store.
//
// MONOLITH DEFECTS, recorded for dev (ID-P8-3) and skipped by name on the monolith arm they live
// on unless MGITEST_RUN_MONOLITH_MIP_DEFECTS=1 is set (which is how each was shown red):
//   M1  Espryt: the RGB16F / RGB32F CPU filter reads the frontend's level shadow, which a GPU
//       write to level 0 leaves stale (DirectGLES.cpp GenerateThreeChannelFloatMipmapOnCpu);
//   M2  Espryt: the R11F / depth blit chains start at level 1 and run to the end of the chain,
//       ignoring BASE_LEVEL and MAX_LEVEL, and the storage grow before them defines levels past
//       MAX_LEVEL (DirectGLES.cpp GenerateColorTexture2DMipmap / GenerateDepthTexture2DMipmap /
//       EnsureGenerateMipmapStorageAllocated);
//   M3  Magma: a colour format without BLIT_SRC|BLIT_DST (R11F_G11F_B10F on lavapipe) is skipped
//       with a warning and its levels stay undefined texels - the monolith has no shader arm,
//       which the wire arm has had since P7 (VulkanRenderer.cpp GenerateMipmap).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

#include "../Harness/HeadlessGL.h"
#include "../Harness/PipeStatsWindow.h"
#include "../Harness/ScenarioFixture.h"
#include "../Harness/SplitRuntimePeek.h"

#ifdef GLAPI
#undef GLAPI
#endif
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glcorearb.h>
#undef GL_GLEXT_PROTOTYPES

namespace MGITest {
    namespace {

        constexpr int kEdge = 16;
        constexpr const char* kForceCpuMipmap = "MGITEST_ESPRYT_FORCE_CPU_MIPMAP";
        constexpr const char* kCpuMipmapMarker = "generate-mipmap-server-cpu";
        constexpr const char* kRunMonolithDefects = "MGITEST_RUN_MONOLITH_MIP_DEFECTS";
        // P8-SE: E's server-read knob; the entries set it to kEdge x kEdge, the base level's extent.
        constexpr const char* kRefuseReadback = "MGITEST_ESPRYT_REFUSE_TEXTURE_READBACK_EXTENT";
        constexpr const char* kStoreDeclinedMarker = "generate-mipmap-store-declined";
        constexpr const char* kStoreFilteredMarker = "filtering the staged store's copy";

        constexpr const char* kVertexSource = R"(#version 430 core
void main() {
    vec2 positions[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    gl_Position = vec4(positions[gl_VertexID], 0.0, 1.0);
}
)";
        constexpr const char* kSampleSource = R"(#version 430 core
uniform sampler2D u_texture;
uniform int u_level;
out vec4 o_color;
void main() { o_color = texelFetch(u_texture, ivec2(gl_FragCoord.xy), u_level); }
)";
        constexpr const char* kPaintSource = R"(#version 430 core
uniform vec4 u_bottom;
uniform vec4 u_top;
uniform float u_split;
out vec4 o_color;
void main() { o_color = gl_FragCoord.y < u_split ? u_bottom : u_top; }
)";

        using Rgb = std::array<float, 3>;
        // Exact in R11F_G11F_B10F, RGB16F and RGB32F alike, and apart in every channel.
        constexpr Rgb kBottom = {0.25f, 0.5f, 0.75f};
        constexpr Rgb kTop = {0.875f, 0.375f, 0.125f};
        constexpr Rgb kSentinel = {1.0f, 0.0f, 1.0f};
        constexpr Rgb kLevelZero = {1.0f, 0.0f, 0.0f};
        constexpr Rgb kLevelTwo = {0.0f, 0.0f, 1.0f};
        constexpr Rgb kLevelThree = {1.0f, 1.0f, 0.0f};
        constexpr float kDepthBottom = 0.25f;
        constexpr float kDepthTop = 0.75f;
        constexpr float kDepthLevelZero = 0.125f;
        constexpr float kDepthLevelTwo = 0.5f;
        constexpr float kDepthLevelThree = 1.0f;

        struct Format {
            GLenum internalFormat;
            bool depth;
            const char* name;
        };
        constexpr Format kPackedFloat = {GL_R11F_G11F_B10F, false, "R11F_G11F_B10F"};
        constexpr Format kDepth32F = {GL_DEPTH_COMPONENT32F, true, "DEPTH_COMPONENT32F"};
        constexpr Format kRgb16F = {GL_RGB16F, false, "RGB16F"};
        constexpr Format kRgb32F = {GL_RGB32F, false, "RGB32F"};

        int ToByte(float value) { return static_cast<int>(std::lround(value * 255.0f)); }

        class GenerateMipmapServerScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                m_wire = SplitRuntimeSkipReason().empty();
                m_espryt = Gl().BackendName() == "DirectGLES";
                m_forcedCpu = std::getenv(kForceCpuMipmap) != nullptr;
                m_refusedReadback = std::getenv(kRefuseReadback) != nullptr;
                std::string error;
                m_sampleProgram = CompileProgram(kVertexSource, kSampleSource, &error);
                ASSERT_NE(m_sampleProgram, 0u) << error;
                m_paintProgram = CompileProgram(kVertexSource, kPaintSource, &error);
                ASSERT_NE(m_paintProgram, 0u) << error;
                m_target = MakeColorFbo(kEdge, kEdge);
                ASSERT_NE(m_target.fbo, 0u) << "could not create the sampling target";
                glGenVertexArrays(1, &m_vao);
                glGenFramebuffers(1, &m_fbo);
                glDisable(GL_SCISSOR_TEST);
                glDisable(GL_DEPTH_TEST);
                glDisable(GL_BLEND);
                glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                ASSERT_EQ(FirstGLError(), 0u);
            }

            void TearDown() override {
                if (!Ready()) return;
                glUseProgram(0);
                glBindVertexArray(0);
                glBindTexture(GL_TEXTURE_2D, 0);
                if (m_sampleProgram != 0) glDeleteProgram(m_sampleProgram);
                if (m_paintProgram != 0) glDeleteProgram(m_paintProgram);
                if (m_vao != 0) glDeleteVertexArrays(1, &m_vao);
                if (m_fbo != 0) glDeleteFramebuffers(1, &m_fbo);
                if (m_texture != 0) glDeleteTextures(1, &m_texture);
                BindDefaultFramebuffer();
                if (m_target.fbo != 0) DestroyColorFbo(m_target);
                glViewport(0, 0, Gl().Width(), Gl().Height());
                FirstGLError();
            }

            // Whether this case meets a recorded monolith defect on this arm (see the header). The
            // knob runs it anyway, which is how each defect is shown red and how dev's fix is checked.
            bool MonolithDefect(bool onEspryt, bool onMagma) const {
                if (m_wire || std::getenv(kRunMonolithDefects) != nullptr) return false;
                return m_espryt ? onEspryt : onMagma;
            }
            static constexpr const char* kM1 =
                "monolith defect M1 (recorded for dev, ID-P8-3): Espryt's CPU filter reads the frontend's "
                "level shadow, which the draw into level 0 left stale";
            static constexpr const char* kM2 =
                "monolith defect M2 (recorded for dev, ID-P8-3): Espryt's blit chain ignores BASE_LEVEL and "
                "MAX_LEVEL and its storage grow defines levels past MAX_LEVEL";
            static constexpr const char* kM3 =
                "monolith defect M3 (recorded for dev, ID-P8-3): Magma skips a format without native blit "
                "and leaves the generated levels empty";

            GLuint NewTexture() {
                glGenTextures(1, &m_texture);
                glBindTexture(GL_TEXTURE_2D, m_texture);
                return m_texture;
            }

            // A level `edge` x `edge` whose rows below the middle are `bottom` and above it `top`.
            static std::vector<float> Halves(const Format& format, int edge, const Rgb& bottom, const Rgb& top,
                                             float depthBottom, float depthTop) {
                const int channels = format.depth ? 1 : 3;
                std::vector<float> texels(static_cast<std::size_t>(edge * edge * channels));
                for (int y = 0; y < edge; ++y) {
                    for (int x = 0; x < edge; ++x) {
                        float* texel = texels.data() + static_cast<std::size_t>((y * edge + x) * channels);
                        const bool low = y < edge / 2;
                        if (format.depth) {
                            texel[0] = low ? depthBottom : depthTop;
                        } else {
                            for (int c = 0; c < 3; ++c) texel[c] = low ? bottom[c] : top[c];
                        }
                    }
                }
                return texels;
            }

            static std::vector<float> Flat(const Format& format, int edge, const Rgb& colour, float depth) {
                return Halves(format, edge, colour, colour, depth, depth);
            }

            static void DefineLevel(const Format& format, int level, int edge, const std::vector<float>& texels) {
                glTexImage2D(GL_TEXTURE_2D, level, static_cast<GLint>(format.internalFormat), edge, edge, 0,
                             format.depth ? GL_DEPTH_COMPONENT : GL_RGB, GL_FLOAT, texels.data());
            }

            static void UploadLevel(const Format& format, int level, int edge, const std::vector<float>& texels) {
                glTexSubImage2D(GL_TEXTURE_2D, level, 0, 0, edge, edge, format.depth ? GL_DEPTH_COMPONENT : GL_RGB,
                                GL_FLOAT, texels.data());
            }

            static int LevelWidth(int level) {
                GLint width = 0;
                glGetTexLevelParameteriv(GL_TEXTURE_2D, level, GL_TEXTURE_WIDTH, &width);
                return width;
            }

            // texelFetch's level is relative to BASE_LEVEL and bounded by MAX_LEVEL, so the window is
            // reopened over every defined level before any of them is read.
            Image SampleLevel(int level, int maxLevel) {
                const int edge = std::max(kEdge >> level, 1);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, m_texture);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, maxLevel);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glUseProgram(m_sampleProgram);
                glUniform1i(glGetUniformLocation(m_sampleProgram, "u_texture"), 0);
                glUniform1i(glGetUniformLocation(m_sampleProgram, "u_level"), level);
                BindFbo(m_target);
                glViewport(0, 0, edge, edge);
                glBindVertexArray(m_vao);
                glDrawArrays(GL_TRIANGLES, 0, 3);
                return ReadPixelsRect(0, 0, edge, edge);
            }

            // Every row of `image` against the half it belongs to. A depth level is read in its red
            // channel only, and only in its bottom and top rows: the rows next to the boundary are
            // where a shader-based depth mip is entitled to a half-texel offset.
            static void ExpectHalves(const Image& image, const Format& format, const Rgb& bottom, const Rgb& top,
                                     float depthBottom, float depthTop, const std::string& what) {
                const int height = image.Height();
                for (int y = 0; y < height; ++y) {
                    if (format.depth && y != 0 && y != height - 1) continue;
                    const bool low = y < height / 2;
                    for (int x = 0; x < image.Width(); ++x) {
                        const Rgba8 got = image.At(x, y);
                        if (format.depth) {
                            const int want = ToByte(low ? depthBottom : depthTop);
                            EXPECT_NEAR(got.r, want, 2) << what << " depth at (" << x << "," << y << ") got " << got;
                            if (std::abs(got.r - want) > 2) return;
                            continue;
                        }
                        const Rgb& colour = low ? bottom : top;
                        const int want[3] = {ToByte(colour[0]), ToByte(colour[1]), ToByte(colour[2])};
                        const int have[3] = {got.r, got.g, got.b};
                        for (int c = 0; c < 3; ++c) {
                            EXPECT_NEAR(have[c], want[c], 2) << what << " channel " << c << " at (" << x << ","
                                                             << y << ") got " << got;
                            if (std::abs(have[c] - want[c]) > 2) return;
                        }
                    }
                }
            }

            static void ExpectFlat(const Image& image, const Format& format, const Rgb& colour, float depth,
                                   const std::string& what) {
                ExpectHalves(image, format, colour, colour, depth, depth, what);
            }

            void Generate() {
                m_mark = PipeStatsWindow::MarkLaneLog();
                glBindTexture(GL_TEXTURE_2D, m_texture);
                glGenerateMipmap(GL_TEXTURE_2D);
                ASSERT_EQ(FirstGLError(), 0u) << "glGenerateMipmap";
            }

            // Under the knob the SERVER must say it filtered on the CPU; without that line the entry
            // ran the native arm and its green says nothing about the arm it is named for.
            void ExpectCpuArmNamed() {
                if (!m_forcedCpu || !m_wire || !m_espryt) return;
                const std::string log = PipeStatsWindow::ReadServerLogSince(m_mark);
                EXPECT_NE(log.find(kCpuMipmapMarker), std::string::npos)
                    << kForceCpuMipmap << " is set but the server log after glGenerateMipmap carries no '"
                    << kCpuMipmapMarker << "' line: the knob did not reach the server, or the three-channel "
                    << "generation never took the server's CPU arm";
            }

            // ---- B1: the generated window, on the two formats Espryt blits level by level ----------

            // Levels 0..2 defined, MAX_LEVEL = 2: the descriptor holds three of the chain's five levels.
            void MutableMaxLevelTwo(const Format& format) {
                if (MonolithDefect(true, !format.depth)) GTEST_SKIP() << (m_espryt ? kM2 : kM3);
                NewTexture();
                DefineLevel(format, 0, kEdge, Halves(format, kEdge, kBottom, kTop, kDepthBottom, kDepthTop));
                DefineLevel(format, 1, kEdge / 2, Flat(format, kEdge / 2, kSentinel, kDepthLevelZero));
                DefineLevel(format, 2, kEdge / 4, Flat(format, kEdge / 4, kSentinel, kDepthLevelZero));
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 2);
                ASSERT_EQ(FirstGLError(), 0u) << "define levels 0..2";
                Generate();
                EXPECT_EQ(LevelWidth(3), 0) << format.name << ": level 3 is past MAX_LEVEL and must stay undefined";
                for (int level = 1; level <= 2; ++level)
                    ExpectHalves(SampleLevel(level, 2), format, kBottom, kTop, kDepthBottom, kDepthTop,
                                 std::string(format.name) + " MAX_LEVEL=2 level " + std::to_string(level));
                ExpectHalves(SampleLevel(0, 2), format, kBottom, kTop, kDepthBottom, kDepthTop,
                             std::string(format.name) + " MAX_LEVEL=2 level 0 (the base, unchanged)");
                EXPECT_EQ(FirstGLError(), 0u);
            }

            // glTexStorage2D with three of the chain's five levels.
            void PartialStorage(const Format& format) {
                if (MonolithDefect(false, !format.depth)) GTEST_SKIP() << kM3;
                NewTexture();
                glTexStorage2D(GL_TEXTURE_2D, 3, format.internalFormat, kEdge, kEdge);
                UploadLevel(format, 0, kEdge, Halves(format, kEdge, kBottom, kTop, kDepthBottom, kDepthTop));
                ASSERT_EQ(FirstGLError(), 0u) << "partial storage";
                Generate();
                for (int level = 1; level <= 2; ++level)
                    ExpectHalves(SampleLevel(level, 2), format, kBottom, kTop, kDepthBottom, kDepthTop,
                                 std::string(format.name) + " three-level storage level " + std::to_string(level));
                EXPECT_EQ(FirstGLError(), 0u);
            }

            // BASE_LEVEL = 1, MAX_LEVEL = 2 over four defined levels: only level 2 is generated, from
            // level 1; levels 0 and 3 keep what they held and level 4 is never defined.
            void MutableBaseLevelOne(const Format& format) {
                if (MonolithDefect(true, !format.depth)) GTEST_SKIP() << (m_espryt ? kM2 : kM3);
                NewTexture();
                DefineLevel(format, 0, kEdge, Flat(format, kEdge, kLevelZero, kDepthLevelZero));
                DefineLevel(format, 1, kEdge / 2,
                            Halves(format, kEdge / 2, kBottom, kTop, kDepthBottom, kDepthTop));
                DefineLevel(format, 2, kEdge / 4, Flat(format, kEdge / 4, kLevelTwo, kDepthLevelTwo));
                DefineLevel(format, 3, kEdge / 8, Flat(format, kEdge / 8, kLevelThree, kDepthLevelThree));
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 1);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 2);
                ASSERT_EQ(FirstGLError(), 0u) << "define levels 0..3";
                Generate();
                EXPECT_EQ(LevelWidth(4), 0) << format.name << ": level 4 is past MAX_LEVEL and must stay undefined";
                const std::string name(format.name);
                ExpectFlat(SampleLevel(0, 3), format, kLevelZero, kDepthLevelZero, name + " level 0 (below BASE_LEVEL)");
                ExpectHalves(SampleLevel(1, 3), format, kBottom, kTop, kDepthBottom, kDepthTop, name + " level 1 (base)");
                ExpectHalves(SampleLevel(2, 3), format, kBottom, kTop, kDepthBottom, kDepthTop,
                             name + " level 2 (generated from level 1)");
                ExpectFlat(SampleLevel(3, 3), format, kLevelThree, kDepthLevelThree,
                           name + " level 3 (past MAX_LEVEL)");
                EXPECT_EQ(FirstGLError(), 0u);
            }

            // ---- B2: RGB16F / RGB32F ----------------------------------------------------------------

            void UploadedBase(const Format& format) {
                NewTexture();
                DefineLevel(format, 0, kEdge, Halves(format, kEdge, kBottom, kTop, 0, 0));
                ASSERT_EQ(FirstGLError(), 0u) << "define level 0";
                Generate();
                const int last = 4;
                for (int level = 1; level < last; ++level)
                    ExpectHalves(SampleLevel(level, last), format, kBottom, kTop, 0, 0,
                                 std::string(format.name) + " uploaded base, level " + std::to_string(level));
                EXPECT_EQ(LevelWidth(last), 1) << format.name << ": the generated chain ends at 1x1";
                ExpectCpuArmNamed();
                EXPECT_EQ(FirstGLError(), 0u);
            }

            // Complementary's colortex2 shape: the base is a render target the GPU wrote after its one
            // upload, so the chain must come from the GPU's level 0 and never from the uploaded texels.
            void RenderTargetBase(const Format& format) {
                if (MonolithDefect(true, false)) GTEST_SKIP() << kM1;
                PaintSentinelBaseAsRenderTarget(format);
                if (HasFatalFailure() || IsSkipped()) return;
                Generate();
                const int last = 4;
                for (int level = 0; level < last; ++level)
                    ExpectHalves(SampleLevel(level, last), format, kBottom, kTop, 0, 0,
                                 std::string(format.name) + " render-target base, level " + std::to_string(level));
                ExpectCpuArmNamed();
                EXPECT_EQ(FirstGLError(), 0u);
            }

            // ---- P8-SE: the CPU arm's store route under a refused readback ----------------------

            // Only the `CpuMipRefused.` entries carry both server knobs; anywhere else the store route
            // is unreachable on a host (its driver reads every level back) and the case says so.
            void SkipUnlessTheStoreRouteIsArmed() {
                if (m_wire && m_espryt && m_forcedCpu && m_refusedReadback) return;
                GTEST_SKIP() << "the store route needs Espryt's split server with " << kForceCpuMipmap << " and "
                             << kRefuseReadback << "=" << kEdge << "x" << kEdge << " (the CpuMipRefused. entries)";
            }

            // The render-target base again, with the driver refusing to read level 0 back: the store
            // holds the uploaded sentinel, the driver the drawn halves, and P8-E's mark says so. The
            // arm must decline by name - a stale chain of sentinel texels is the defect.
            void RefusedReadbackOfADrawnBase(const Format& format) {
                SkipUnlessTheStoreRouteIsArmed();
                if (IsSkipped()) return;
                PaintSentinelBaseAsRenderTarget(format);
                if (HasFatalFailure() || IsSkipped()) return;
                Generate();
                const std::string log = PipeStatsWindow::ReadServerLogSince(m_mark);
                EXPECT_NE(log.find(kStoreDeclinedMarker), std::string::npos)
                    << "the base level was drawn and its readback refused, but the server log after glGenerateMipmap "
                       "has no '" << kStoreDeclinedMarker << "' line";
                EXPECT_EQ(log.find(kStoreFilteredMarker), std::string::npos)
                    << "the CPU arm filtered the staged store's copy of a level the driver has since drawn over";
                const int last = 4;
                ExpectHalves(SampleLevel(0, last), format, kBottom, kTop, 0, 0,
                             std::string(format.name) + " drawn base (untouched by the decline)");
                for (int level = 1; level < last; ++level) {
                    const Image image = SampleLevel(level, last);
                    const Rgba8 got = image.At(0, 0);
                    EXPECT_FALSE(std::abs(got.r - ToByte(kSentinel[0])) <= 2 && std::abs(got.g - ToByte(kSentinel[1])) <= 2 &&
                                 std::abs(got.b - ToByte(kSentinel[2])) <= 2)
                        << format.name << " level " << level << " is the uploaded sentinel " << got
                        << ": the chain was filtered from the stale staged store";
                }
                ExpectCpuArmNamed();
                FirstGLError();
            }

            // The control: a base that only ever crossed as an upload IS what the store holds, so a
            // refused readback is answered from the store and the chain is right. This is what tells
            // the check above from "the store route always declines" - the generation's own
            // driver-write note must not count against the level it reads.
            void RefusedReadbackOfAnUploadedBase(const Format& format) {
                SkipUnlessTheStoreRouteIsArmed();
                if (IsSkipped()) return;
                NewTexture();
                DefineLevel(format, 0, kEdge, Halves(format, kEdge, kBottom, kTop, 0, 0));
                ASSERT_EQ(FirstGLError(), 0u) << "define level 0";
                Generate();
                const std::string log = PipeStatsWindow::ReadServerLogSince(m_mark);
                EXPECT_NE(log.find(kStoreFilteredMarker), std::string::npos)
                    << "the uploaded base's readback was refused, but the server log has no '" << kStoreFilteredMarker
                    << "' line: the store route did not answer";
                EXPECT_EQ(log.find(kStoreDeclinedMarker), std::string::npos)
                    << "the store route declined a level that only ever crossed as an upload";
                const int last = 4;
                for (int level = 1; level < last; ++level)
                    ExpectHalves(SampleLevel(level, last), format, kBottom, kTop, 0, 0,
                                 std::string(format.name) + " uploaded base, store-filtered level " +
                                     std::to_string(level));
                EXPECT_EQ(FirstGLError(), 0u);
            }

            // Level 0 uploaded as the sentinel, then drawn over as a render target (bottom / top halves).
            void PaintSentinelBaseAsRenderTarget(const Format& format) {
                NewTexture();
                DefineLevel(format, 0, kEdge, Flat(format, kEdge, kSentinel, 0));
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_texture, 0);
                const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
                if (status != GL_FRAMEBUFFER_COMPLETE) {
                    glBindFramebuffer(GL_FRAMEBUFFER, 0);
                    FirstGLError();
                    GTEST_SKIP() << format.name << " is not colour-renderable on " << Gl().BackendName()
                                 << " (status 0x" << std::hex << status << ")";
                }
                glViewport(0, 0, kEdge, kEdge);
                glUseProgram(m_paintProgram);
                glUniform4f(glGetUniformLocation(m_paintProgram, "u_bottom"), kBottom[0], kBottom[1], kBottom[2], 1);
                glUniform4f(glGetUniformLocation(m_paintProgram, "u_top"), kTop[0], kTop[1], kTop[2], 1);
                glUniform1f(glGetUniformLocation(m_paintProgram, "u_split"), kEdge / 2.0f);
                glBindVertexArray(m_vao);
                glDrawArrays(GL_TRIANGLES, 0, 3);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                ASSERT_EQ(FirstGLError(), 0u) << "draw into level 0";
            }

            bool m_wire = false;
            bool m_espryt = false;
            bool m_forcedCpu = false;
            bool m_refusedReadback = false;
            GLuint m_sampleProgram = 0;
            GLuint m_paintProgram = 0;
            GLuint m_vao = 0;
            GLuint m_fbo = 0;
            GLuint m_texture = 0;
            ColorFbo m_target{};
            PipeStatsWindow::LogMark m_mark{};
        };

    } // namespace

    // B1.
    TEST_F(GenerateMipmapServerScenario, PackedFloatMutableMaxLevelBelowTheChain) {
        if (!Ready()) return;
        MutableMaxLevelTwo(kPackedFloat);
    }
    TEST_F(GenerateMipmapServerScenario, PackedFloatStorageWithFewerLevelsThanTheChain) {
        if (!Ready()) return;
        PartialStorage(kPackedFloat);
    }
    TEST_F(GenerateMipmapServerScenario, PackedFloatMutableBaseLevelOneKeepsTheLevelsOutsideItsWindow) {
        if (!Ready()) return;
        MutableBaseLevelOne(kPackedFloat);
    }
    TEST_F(GenerateMipmapServerScenario, DepthMutableMaxLevelBelowTheChain) {
        if (!Ready()) return;
        MutableMaxLevelTwo(kDepth32F);
    }
    TEST_F(GenerateMipmapServerScenario, DepthStorageWithFewerLevelsThanTheChain) {
        if (!Ready()) return;
        PartialStorage(kDepth32F);
    }
    TEST_F(GenerateMipmapServerScenario, DepthMutableBaseLevelOneKeepsTheLevelsOutsideItsWindow) {
        if (!Ready()) return;
        MutableBaseLevelOne(kDepth32F);
    }

    // B2.
    TEST_F(GenerateMipmapServerScenario, Rgb16fUploadedBaseGeneratesItsChain) {
        if (!Ready()) return;
        UploadedBase(kRgb16F);
    }
    TEST_F(GenerateMipmapServerScenario, Rgb16fRenderTargetBaseGeneratesFromTheGpuWrittenLevel) {
        if (!Ready()) return;
        RenderTargetBase(kRgb16F);
    }
    TEST_F(GenerateMipmapServerScenario, Rgb32fUploadedBaseGeneratesItsChain) {
        if (!Ready()) return;
        UploadedBase(kRgb32F);
    }
    TEST_F(GenerateMipmapServerScenario, Rgb32fRenderTargetBaseGeneratesFromTheGpuWrittenLevel) {
        if (!Ready()) return;
        RenderTargetBase(kRgb32F);
    }

    // P8-SE: the CPU arm's store route (header; the CpuMipRefused. entries).
    TEST_F(GenerateMipmapServerScenario, Rgb16fDrawnBaseWithARefusedReadbackDeclinesByName) {
        if (!Ready()) return;
        RefusedReadbackOfADrawnBase(kRgb16F);
    }
    TEST_F(GenerateMipmapServerScenario, Rgb16fUploadedBaseWithARefusedReadbackIsFilteredFromTheStore) {
        if (!Ready()) return;
        RefusedReadbackOfAnUploadedBase(kRgb16F);
    }

} // namespace MGITest
