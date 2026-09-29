// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/TextureRemintPullScenario.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - A TEXTURE RE-MINT NEEDS NO BYTE FROM THE CLIENT (P9 W2, notes/p9/W2-REMINT.md).
//
// The name is the ROADMAP's: P4a named `texture-remint-pull` the one new stall class of the
// split - a texture first bound to an image unit AFTER the backend allocated it has to be
// re-minted in image-bindable (possibly channel-widened) storage, and design/04 §8.4 assumed the
// server held no texels to fill it with, so the levels would be PULLED back from the client. P9
// W2 measured the opposite on both backends: Espryt's server reads each level back from the old
// GPU texture and replays its own staged store for what is still pending
// (RequireImageBindableStorageByHandle), Magma copies the old image into the new one GPU to GPU
// (PreserveTextureContentsOnRecreate). No pull protocol exists, and this scenario is what says
// the re-mint is right without one.
//
// EVERY CASE HAS ONE SHAPE. Give a texture its content by one means, sync it to the backend with
// a sampling draw (so the backend really holds storage when the bind arrives - a texture whose
// first sync comes after the bind is allocated image-bindable up front and re-mints nothing),
// optionally write it on the GPU, then glBindImageTexture it and copy it with imageLoad into a
// destination that was image-bound BEFORE its own first sync (so the destination never re-mints),
// and read the destination back through a framebuffer. The expectation is the GL answer, never
// "what monolith does": the cases exist exactly where the arms can differ.
//
//   A   mutable, uploaded, synced                         the store and the GPU agree
//   B   mutable, uploaded, then cleared on the GPU        the store is STALE, the GPU holds it
//   C   immutable RGBA8, uploaded, then cleared           Espryt's split keeps the allocation
//   D   null-defined, only ever cleared on the GPU        the store has NO bytes
//   E   cleared, then an unsynced partial upload          GPU texels merged with the pending box
//   F   glGenerateMipmap's level 1                        GPU-generated, no bytes ever crossed
//   F2  an uploaded level 1, then cleared on the GPU      as B, one level down
//   G   level 1 null-defined AFTER the last sync          the driver does not hold the level
//   G2  level 0 redefined larger, null, after the sync    the driver holds the OLD extent
//   G3  G, plus a partial upload into that level          the store holds the whole level
//   H   widened rg8 immutable, uploaded, then cleared     the carrier re-mint (Espryt promotes)
//   H2  widened rg8, uploaded only
//   I   RGBA8_SNORM mutable, uploaded only
//   J   RGBA16_SNORM immutable (widened), uploaded only
//
// G and G3 were Fatal{ResourceUnavailable, "image-promotion-readback"} on Espryt's split arms
// until W2-a: the promotion read a level the driver never allocated.
//
// ARMS. Both backends on the monolith arm and on Split / Spawn / Tcp. ONE NAMED SKIP: the
// monolith DirectGLES arm replays the client's shadow over texels the GPU wrote (B, C, D, E, F,
// F2, H) - a known dev defect, not P9's (W2-REMINT.md §6), whose fix moves the pull build's .text
// (G1). Its other cases run and pass there.
//
// W2-d's COUNTER. `trp=` (TextureRemintPulls) counts a re-mint of storage the backend already
// held on BOTH Espryt arms now. The peek is process-wide, so it sees the server's count only
// where the server is this process: monolith and inproc. A/H assert +1 there, C asserts +0 on
// inproc (the kept allocation is not a re-mint).
//
// W2-b's CONTROL. MGITEST_ESPRYT_FORCE_REMINT_READBACK_FAILURE=1 (read by the SERVER) makes every
// promotion readback fail, which a host driver cannot be made to do for real. The
// `DirectGLES.{Split,Spawn}.RemintFallback.` entries run the uploaded-only cases under it: the
// store covers those levels, so the pixels stay right and the server log names the fallback.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "../Harness/HeadlessGL.h"
#include "../Harness/P4aFinalFixPeek.h"
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

        constexpr int kEdge = 8;
        constexpr const char* kForceReadbackFailure = "MGITEST_ESPRYT_FORCE_REMINT_READBACK_FAILURE";
        constexpr const char* kFallbackMarker = "remint-readback-fallback";

        constexpr const char* kVertexSource = R"(#version 430 core
void main() {
    vec2 positions[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    gl_Position = vec4(positions[gl_VertexID], 0.0, 1.0);
}
)";
        // The sync: any draw that samples the texture makes the backend allocate and upload it.
        constexpr const char* kSampleSource = R"(#version 430 core
uniform sampler2D u_texture;
out vec4 o_color;
void main() { o_color = texelFetch(u_texture, ivec2(gl_FragCoord.xy) % 4, 0); }
)";

        std::string CopyComputeSource(const char* sourceFormat, bool signedNormalized) {
            std::string source = "#version 430 core\nlayout(local_size_x = 8, local_size_y = 8) in;\n";
            source += std::string("layout(") + sourceFormat + ", binding = 0) readonly uniform image2D u_source;\n";
            source += "layout(rgba8, binding = 1) writeonly uniform image2D u_destination;\n";
            source += "void main() {\n    ivec2 p = ivec2(gl_GlobalInvocationID.xy);\n";
            source += "    vec4 v = imageLoad(u_source, p);\n";
            if (signedNormalized) source += "    v = v * 0.5 + 0.5;\n";
            source += "    imageStore(u_destination, p, v);\n}\n";
            return source;
        }

        std::vector<GLubyte> Pattern(int edge, GLubyte magic) {
            std::vector<GLubyte> texels(static_cast<std::size_t>(edge * edge) * 4u);
            for (int y = 0; y < edge; ++y) {
                for (int x = 0; x < edge; ++x) {
                    const std::size_t at = static_cast<std::size_t>((y * edge + x) * 4);
                    texels[at + 0] = magic;
                    texels[at + 1] = static_cast<GLubyte>(16 * (x + 1));
                    texels[at + 2] = static_cast<GLubyte>(16 * (y + 1));
                    texels[at + 3] = 0xFF;
                }
            }
            return texels;
        }

        std::vector<GLubyte> Solid(int edge, GLubyte r, GLubyte g, GLubyte b, GLubyte a) {
            std::vector<GLubyte> texels(static_cast<std::size_t>(edge * edge) * 4u);
            for (std::size_t i = 0; i < texels.size(); i += 4) {
                texels[i] = r;
                texels[i + 1] = g;
                texels[i + 2] = b;
                texels[i + 3] = a;
            }
            return texels;
        }

        const GLfloat kGreen[4] = {0.0f, 1.0f, 0.0f, 1.0f};

        class TextureRemintPullScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                GLint computeImages = 0;
                glGetIntegerv(GL_MAX_COMPUTE_IMAGE_UNIFORMS, &computeImages);
                FirstGLError();
                if (computeImages < 2) {
                    GTEST_SKIP() << "the compute stage has fewer than two image uniforms";
                }
                m_wire = SplitRuntimeSkipReason().empty();
                m_inproc = m_wire && PeekSplitRuntime().transportName == "inproc";
                m_espryt = Gl().BackendName() == "DirectGLES";
                m_forced = std::getenv(kForceReadbackFailure) != nullptr;
                std::string error;
                m_sampleProgram = CompileProgram(kVertexSource, kSampleSource, &error);
                ASSERT_NE(m_sampleProgram, 0u) << error;
                m_scratch = MakeColorFbo(4, 4);
                ASSERT_NE(m_scratch.fbo, 0u) << "could not create the sampling target";
                glGenVertexArrays(1, &m_vao);
                glGenFramebuffers(1, &m_fbo);
                ASSERT_EQ(FirstGLError(), 0u);
            }

            void TearDown() override {
                if (!Ready()) return;
                glUseProgram(0);
                glBindImageTexture(0, 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);
                glBindImageTexture(1, 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);
                glBindTexture(GL_TEXTURE_2D, 0);
                if (m_sampleProgram != 0) glDeleteProgram(m_sampleProgram);
                if (m_vao != 0) glDeleteVertexArrays(1, &m_vao);
                if (m_fbo != 0) glDeleteFramebuffers(1, &m_fbo);
                if (!m_textures.empty()) {
                    glDeleteTextures(static_cast<GLsizei>(m_textures.size()), m_textures.data());
                }
                BindDefaultFramebuffer();
                if (m_scratch.fbo != 0) DestroyColorFbo(m_scratch);
                glViewport(0, 0, Gl().Width(), Gl().Height());
                FirstGLError();
            }

            // The one named skip (see the header): the monolith DirectGLES re-mint replays the
            // client's shadow, which never saw what the GPU wrote. Empty when the arm is a subject.
            std::string MonolithEsprytShadowReplaySkip() const {
                if (!m_espryt || m_wire) return {};
                return "the monolith DirectGLES re-mint (RequireImageBindableStorage) replays the client's "
                       "shadow over texels the GPU wrote - a known dev defect filed separately "
                       "(notes/p9/W2-REMINT.md §6), whose fix moves the pull build's .text (G1). The split "
                       "arms and both Magma arms are this case's subject.";
            }

            GLuint NewTexture() {
                GLuint texture = 0;
                glGenTextures(1, &texture);
                m_textures.push_back(texture);
                return texture;
            }

            static void NearestWithMaxLevel(GLuint texture, int maxLevel) {
                glBindTexture(GL_TEXTURE_2D, texture);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, maxLevel);
                glBindTexture(GL_TEXTURE_2D, 0);
            }

            // A draw that samples the texture: from here on the backend holds storage for it.
            void SyncBySampling(GLuint texture) {
                BindFbo(m_scratch);
                glUseProgram(m_sampleProgram);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, texture);
                glUniform1i(glGetUniformLocation(m_sampleProgram, "u_texture"), 0);
                glBindVertexArray(m_vao);
                glDrawArrays(GL_TRIANGLES, 0, 3);
                glBindVertexArray(0);
                glBindTexture(GL_TEXTURE_2D, 0);
                glUseProgram(0);
                BindDefaultFramebuffer();
                glFinish();
            }

            // A GPU write the client's shadow never sees.
            void ClearOnTheGpu(GLuint texture, int level, const GLfloat* color) {
                glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, level);
                ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GLenum(GL_FRAMEBUFFER_COMPLETE));
                glClearBufferfv(GL_COLOR, 0, color);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
                BindDefaultFramebuffer();
            }

            // THE TRANSITION AND THE READ. The destination is image-bound before its first sync,
            // so only the source can re-mint; the copy goes through imageLoad, which reads the
            // re-minted storage and nothing else.
            std::vector<GLubyte> ImageCopy(GLuint source, int level, GLenum unitFormat, const char* glslFormat,
                                           bool signedNormalized = false) {
                const GLuint destination = NewTexture();
                glBindTexture(GL_TEXTURE_2D, destination);
                glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, kEdge, kEdge);
                glBindTexture(GL_TEXTURE_2D, 0);
                glBindImageTexture(1, destination, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
                glBindImageTexture(0, source, level, GL_FALSE, 0, GL_READ_ONLY, unitFormat);
                EXPECT_EQ(FirstGLError(), 0u) << "glBindImageTexture raised a GL error";
                const std::string computeSource = CopyComputeSource(glslFormat, signedNormalized);
                const char* text = computeSource.c_str();
                const GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
                glShaderSource(shader, 1, &text, nullptr);
                glCompileShader(shader);
                const GLuint program = glCreateProgram();
                glAttachShader(program, shader);
                glLinkProgram(program);
                glDeleteShader(shader);
                GLint linked = GL_FALSE;
                glGetProgramiv(program, GL_LINK_STATUS, &linked);
                EXPECT_EQ(linked, GL_TRUE) << "the imageLoad copy did not link";
                glUseProgram(program);
                // A BARRIER THE SPEC DOES NOT ASK FOR, AND IT IS HERE ON PURPOSE. GL orders a
                // framebuffer write (the GPU clears above) before a later shader image load without
                // one, yet on the Espryt split arm the kept-allocation case (C, no re-mint to
                // serialize anything) read the pre-clear texels in 3 of 30 runs without it and 0 of
                // 30 with it, while every server-side decision was identical in passing and failing
                // runs (W2-REMINT.md §7). That ordering gap is not the re-mint's question; this case
                // asks what the re-minted storage HOLDS, so the ordering is pinned here.
                glMemoryBarrier(GL_ALL_BARRIER_BITS);
                glDispatchCompute(1, 1, 1);
                glMemoryBarrier(GL_ALL_BARRIER_BITS);
                glUseProgram(0);
                glDeleteProgram(program);
                glBindImageTexture(0, 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);
                glBindImageTexture(1, 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);
                glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, destination, 0);
                std::vector<GLubyte> read(static_cast<std::size_t>(kEdge * kEdge) * 4u, 0xA5);
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glReadPixels(0, 0, kEdge, kEdge, GL_RGBA, GL_UNSIGNED_BYTE, read.data());
                glPixelStorei(GL_PACK_ALIGNMENT, 4);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
                BindDefaultFramebuffer();
                EXPECT_EQ(FirstGLError(), 0u) << "the copy or its readback raised a GL error";
                return read;
            }

            // The `edge` x `edge` corner of the read against `want`, `channels` channels, within 2.
            void ExpectTexels(const char* what, const std::vector<GLubyte>& read, int edge,
                              const std::vector<GLubyte>& want, int channels = 4) {
                int wrong = 0;
                int firstX = -1, firstY = -1;
                for (int y = 0; y < edge; ++y) {
                    for (int x = 0; x < edge; ++x) {
                        for (int c = 0; c < channels; ++c) {
                            const int got = read[static_cast<std::size_t>((y * kEdge + x) * 4 + c)];
                            const int expected = want[static_cast<std::size_t>((y * edge + x) * 4 + c)];
                            if (std::abs(got - expected) > 2 && wrong++ == 0) {
                                firstX = x;
                                firstY = y;
                            }
                        }
                    }
                }
                const std::size_t readAt = static_cast<std::size_t>((std::max(firstY, 0) * kEdge + std::max(firstX, 0)) * 4);
                const std::size_t wantAt = static_cast<std::size_t>((std::max(firstY, 0) * edge + std::max(firstX, 0)) * 4);
                EXPECT_EQ(wrong, 0) << what << ": " << wrong << " channel(s) differ; first at (" << firstX << ", "
                                    << firstY << ") read " << int(read[readAt]) << "," << int(read[readAt + 1]) << ","
                                    << int(read[readAt + 2]) << "," << int(read[readAt + 3]) << " want "
                                    << int(want[wantAt]) << "," << int(want[wantAt + 1]) << ","
                                    << int(want[wantAt + 2]) << "," << int(want[wantAt + 3]) << " (backend "
                                    << Gl().BackendName() << ", " << (m_wire ? "split" : "monolith") << ")";
            }

            // W2-d: where the peek can see the server's count (monolith, inproc).
            bool RemintCountVisible() const { return m_espryt && (!m_wire || m_inproc); }
            unsigned long long RemintCount() {
                unsigned long long count = 0;
                EXPECT_TRUE(PeekPipeStatsTextureRemintPulls(&count));
                return count;
            }

            // W2-b: under the forced-failure knob the server must have named its fallback.
            void ExpectFallbackNamedSince(const PipeStatsWindow::LogMark& mark) {
                if (!m_forced) return;
                const std::string log = PipeStatsWindow::ReadServerLogSince(mark);
                EXPECT_NE(log.find(kFallbackMarker), std::string::npos)
                    << kForceReadbackFailure << " is set but the server log after the image bind carries no '"
                    << kFallbackMarker << "' line: either the knob did not reach the server (the entry "
                    << "proves nothing) or the promotion took no readback at all";
            }

            bool m_wire = false;
            bool m_inproc = false;
            bool m_espryt = false;
            bool m_forced = false;
            GLuint m_sampleProgram = 0;
            GLuint m_vao = 0;
            GLuint m_fbo = 0;
            ColorFbo m_scratch{};
            std::vector<GLuint> m_textures;
        };

        TEST_F(TextureRemintPullScenario, AnUploadedMutableTextureKeepsItsTexelsAcrossTheImageBind) {
            if (!Ready()) return;
            const bool counted = RemintCountVisible() && !m_forced;
            const unsigned long long before = counted ? RemintCount() : 0;
            const GLuint texture = NewTexture();
            const auto pattern = Pattern(kEdge, 0x40);
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kEdge, kEdge, 0, GL_RGBA, GL_UNSIGNED_BYTE, pattern.data());
            NearestWithMaxLevel(texture, 0);
            SyncBySampling(texture);
            const auto mark = PipeStatsWindow::MarkLaneLog();
            ExpectTexels("an uploaded mutable texture", ImageCopy(texture, 0, GL_RGBA8, "rgba8"), kEdge, pattern);
            ExpectFallbackNamedSince(mark);
            if (counted) {
                EXPECT_EQ(RemintCount(), before + 1)
                    << "the re-mint of a mutable texture the backend already held was not counted (trp=)";
            }
        }

        TEST_F(TextureRemintPullScenario, AGpuClearAfterTheUploadSurvivesTheImageBind) {
            if (!Ready()) return;
            if (const std::string skip = MonolithEsprytShadowReplaySkip(); !skip.empty()) GTEST_SKIP() << skip;
            const GLuint texture = NewTexture();
            const auto pattern = Pattern(kEdge, 0x40);
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kEdge, kEdge, 0, GL_RGBA, GL_UNSIGNED_BYTE, pattern.data());
            NearestWithMaxLevel(texture, 0);
            SyncBySampling(texture);
            ClearOnTheGpu(texture, 0, kGreen);
            SyncBySampling(texture);
            ExpectTexels("a GPU clear after the upload", ImageCopy(texture, 0, GL_RGBA8, "rgba8"), kEdge,
                         Solid(kEdge, 0, 255, 0, 255));
        }

        TEST_F(TextureRemintPullScenario, AnImmutableTexturesGpuClearSurvivesTheImageBind) {
            if (!Ready()) return;
            if (const std::string skip = MonolithEsprytShadowReplaySkip(); !skip.empty()) GTEST_SKIP() << skip;
            const bool counted = RemintCountVisible() && m_inproc;
            const unsigned long long before = counted ? RemintCount() : 0;
            const GLuint texture = NewTexture();
            const auto pattern = Pattern(kEdge, 0x40);
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, kEdge, kEdge);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kEdge, kEdge, GL_RGBA, GL_UNSIGNED_BYTE, pattern.data());
            NearestWithMaxLevel(texture, 0);
            SyncBySampling(texture);
            ClearOnTheGpu(texture, 0, kGreen);
            SyncBySampling(texture);
            ExpectTexels("an immutable texture's GPU clear", ImageCopy(texture, 0, GL_RGBA8, "rgba8"), kEdge,
                         Solid(kEdge, 0, 255, 0, 255));
            if (counted) {
                EXPECT_EQ(RemintCount(), before)
                    << "an immutable core-format allocation is KEPT by the server, not re-minted, and must "
                       "not count (trp=)";
            }
        }

        TEST_F(TextureRemintPullScenario, ANullDefinedTextureWrittenOnlyByTheGpuSurvivesTheImageBind) {
            if (!Ready()) return;
            if (const std::string skip = MonolithEsprytShadowReplaySkip(); !skip.empty()) GTEST_SKIP() << skip;
            const GLuint texture = NewTexture();
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kEdge, kEdge, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            NearestWithMaxLevel(texture, 0);
            SyncBySampling(texture);
            ClearOnTheGpu(texture, 0, kGreen);
            SyncBySampling(texture);
            ExpectTexels("a null-defined texture only the GPU wrote", ImageCopy(texture, 0, GL_RGBA8, "rgba8"),
                         kEdge, Solid(kEdge, 0, 255, 0, 255));
        }

        TEST_F(TextureRemintPullScenario, APendingPartialUploadMergesOverGpuWrittenTexels) {
            if (!Ready()) return;
            if (const std::string skip = MonolithEsprytShadowReplaySkip(); !skip.empty()) GTEST_SKIP() << skip;
            const GLuint texture = NewTexture();
            const auto pattern = Pattern(kEdge, 0x40);
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kEdge, kEdge, 0, GL_RGBA, GL_UNSIGNED_BYTE, pattern.data());
            NearestWithMaxLevel(texture, 0);
            SyncBySampling(texture);
            ClearOnTheGpu(texture, 0, kGreen);
            SyncBySampling(texture);
            const auto blue = Solid(2, 0, 0, 255, 255);
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 2, 2, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, blue.data());
            glBindTexture(GL_TEXTURE_2D, 0);
            auto want = Solid(kEdge, 0, 255, 0, 255);
            for (int y = 2; y < 4; ++y) {
                for (int x = 2; x < 4; ++x) {
                    const std::size_t at = static_cast<std::size_t>((y * kEdge + x) * 4);
                    want[at] = 0;
                    want[at + 1] = 0;
                    want[at + 2] = 255;
                }
            }
            ExpectTexels("a pending partial upload over GPU texels", ImageCopy(texture, 0, GL_RGBA8, "rgba8"),
                         kEdge, want);
        }

        TEST_F(TextureRemintPullScenario, AGeneratedMipLevelSurvivesTheImageBind) {
            if (!Ready()) return;
            if (const std::string skip = MonolithEsprytShadowReplaySkip(); !skip.empty()) GTEST_SKIP() << skip;
            const GLuint texture = NewTexture();
            const auto solid = Solid(kEdge, 200, 100, 50, 255);
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kEdge, kEdge, 0, GL_RGBA, GL_UNSIGNED_BYTE, solid.data());
            glGenerateMipmap(GL_TEXTURE_2D);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glBindTexture(GL_TEXTURE_2D, 0);
            SyncBySampling(texture);
            ExpectTexels("glGenerateMipmap's level 1", ImageCopy(texture, 1, GL_RGBA8, "rgba8"), kEdge / 2,
                         Solid(kEdge / 2, 200, 100, 50, 255));
        }

        TEST_F(TextureRemintPullScenario, AGpuClearedMipLevelSurvivesTheImageBind) {
            if (!Ready()) return;
            if (const std::string skip = MonolithEsprytShadowReplaySkip(); !skip.empty()) GTEST_SKIP() << skip;
            const GLuint texture = NewTexture();
            const auto level0 = Solid(kEdge, 200, 100, 50, 255);
            const auto level1 = Solid(kEdge / 2, 10, 20, 30, 255);
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kEdge, kEdge, 0, GL_RGBA, GL_UNSIGNED_BYTE, level0.data());
            glTexImage2D(GL_TEXTURE_2D, 1, GL_RGBA8, kEdge / 2, kEdge / 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, level1.data());
            NearestWithMaxLevel(texture, 1);
            SyncBySampling(texture);
            ClearOnTheGpu(texture, 1, kGreen);
            SyncBySampling(texture);
            ExpectTexels("a GPU-cleared level 1", ImageCopy(texture, 1, GL_RGBA8, "rgba8"), kEdge / 2,
                         Solid(kEdge / 2, 0, 255, 0, 255));
        }

        // W2-a. Level 1 exists in the store (a null-data respecify) and on no driver: the texture's
        // last sync predates it. The promotion must not read it back.
        TEST_F(TextureRemintPullScenario, ALevelDefinedWithoutDataAfterTheLastSyncDoesNotEndTheSession) {
            if (!Ready()) return;
            const GLuint texture = NewTexture();
            const auto pattern = Pattern(kEdge, 0x40);
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kEdge, kEdge, 0, GL_RGBA, GL_UNSIGNED_BYTE, pattern.data());
            NearestWithMaxLevel(texture, 0);
            SyncBySampling(texture);
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexImage2D(GL_TEXTURE_2D, 1, GL_RGBA8, kEdge / 2, kEdge / 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glBindTexture(GL_TEXTURE_2D, 0);
            ExpectTexels("level 0 beside a level defined after the last sync",
                         ImageCopy(texture, 0, GL_RGBA8, "rgba8"), kEdge, pattern);
        }

        // W2-a's extent half: the driver holds level 0 at its OLD extent. The new contents are
        // undefined (GL 4.6 core 8.5), so only "no Fatal, no GL error" is asserted.
        TEST_F(TextureRemintPullScenario, ALevelRedefinedLargerWithoutDataAfterTheLastSyncDoesNotEndTheSession) {
            if (!Ready()) return;
            const GLuint texture = NewTexture();
            const auto pattern = Pattern(kEdge / 2, 0x40);
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kEdge / 2, kEdge / 2, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                         pattern.data());
            NearestWithMaxLevel(texture, 0);
            SyncBySampling(texture);
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kEdge, kEdge, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glBindTexture(GL_TEXTURE_2D, 0);
            (void)ImageCopy(texture, 0, GL_RGBA8, "rgba8");
            EXPECT_EQ(FirstGLError(), 0u);
        }

        // W2-a's store half: the level is on no driver, but its whole staged run is in the store
        // (the client's level shadow, the partial upload inside it), and that is what replays.
        TEST_F(TextureRemintPullScenario, APartialUploadIntoALevelDefinedAfterTheLastSyncReachesTheImage) {
            if (!Ready()) return;
            const GLuint texture = NewTexture();
            const auto pattern = Pattern(kEdge, 0x40);
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kEdge, kEdge, 0, GL_RGBA, GL_UNSIGNED_BYTE, pattern.data());
            NearestWithMaxLevel(texture, 0);
            SyncBySampling(texture);
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexImage2D(GL_TEXTURE_2D, 1, GL_RGBA8, kEdge / 2, kEdge / 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            const auto blue = Solid(2, 0, 0, 255, 255);
            glTexSubImage2D(GL_TEXTURE_2D, 1, 0, 0, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, blue.data());
            // Level 1 inside [BASE_LEVEL, MAX_LEVEL], or the image access is invalid (8.26).
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 1);
            glBindTexture(GL_TEXTURE_2D, 0);
            ExpectTexels("a partial upload into a level defined after the last sync",
                         ImageCopy(texture, 1, GL_RGBA8, "rgba8"), 2, blue);
        }

        TEST_F(TextureRemintPullScenario, AWidenedFormatsGpuClearSurvivesTheCarrierRemint) {
            if (!Ready()) return;
            if (const std::string skip = MonolithEsprytShadowReplaySkip(); !skip.empty()) GTEST_SKIP() << skip;
            const bool counted = RemintCountVisible();
            const unsigned long long before = counted ? RemintCount() : 0;
            const GLuint texture = NewTexture();
            std::vector<GLubyte> rg(static_cast<std::size_t>(kEdge * kEdge) * 2u, 0x33);
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_RG8, kEdge, kEdge);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kEdge, kEdge, GL_RG, GL_UNSIGNED_BYTE, rg.data());
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            NearestWithMaxLevel(texture, 0);
            SyncBySampling(texture);
            const GLfloat halfQuarter[4] = {0.5f, 0.25f, 0.0f, 1.0f};
            ClearOnTheGpu(texture, 0, halfQuarter);
            SyncBySampling(texture);
            ExpectTexels("a widened rg8 texture's GPU clear", ImageCopy(texture, 0, GL_RG8, "rg8"), kEdge,
                         Solid(kEdge, 128, 64, 0, 255), 2);
            if (counted) {
                EXPECT_EQ(RemintCount(), before + 1)
                    << "the carrier re-mint of a widened texture the backend already held was not counted "
                       "(trp=)";
            }
        }

        TEST_F(TextureRemintPullScenario, AWidenedFormatsUploadSurvivesTheCarrierRemint) {
            if (!Ready()) return;
            const GLuint texture = NewTexture();
            std::vector<GLubyte> rg(static_cast<std::size_t>(kEdge * kEdge) * 2u);
            for (std::size_t i = 0; i < rg.size(); i += 2) {
                rg[i] = 0x33;
                rg[i + 1] = 0x99;
            }
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_RG8, kEdge, kEdge);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kEdge, kEdge, GL_RG, GL_UNSIGNED_BYTE, rg.data());
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            NearestWithMaxLevel(texture, 0);
            SyncBySampling(texture);
            const auto mark = PipeStatsWindow::MarkLaneLog();
            ExpectTexels("a widened rg8 upload", ImageCopy(texture, 0, GL_RG8, "rg8"), kEdge,
                         Solid(kEdge, 0x33, 0x99, 0, 255), 2);
            ExpectFallbackNamedSince(mark);
        }

        TEST_F(TextureRemintPullScenario, AnRgba8SnormUploadSurvivesTheImageBind) {
            if (!Ready()) return;
            const GLuint texture = NewTexture();
            std::vector<GLbyte> texels(static_cast<std::size_t>(kEdge * kEdge) * 4u);
            for (std::size_t i = 0; i < texels.size(); i += 4) {
                texels[i] = 127;
                texels[i + 1] = 0;
                texels[i + 2] = -127;
                texels[i + 3] = 127;
            }
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8_SNORM, kEdge, kEdge, 0, GL_RGBA, GL_BYTE, texels.data());
            NearestWithMaxLevel(texture, 0);
            SyncBySampling(texture);
            const auto mark = PipeStatsWindow::MarkLaneLog();
            ExpectTexels("an RGBA8_SNORM upload", ImageCopy(texture, 0, GL_RGBA8_SNORM, "rgba8_snorm", true), kEdge,
                         Solid(kEdge, 255, 128, 0, 255), 3);
            ExpectFallbackNamedSince(mark);
        }

        TEST_F(TextureRemintPullScenario, AnRgba16SnormImmutableUploadSurvivesTheCarrierRemint) {
            if (!Ready()) return;
            const GLuint texture = NewTexture();
            std::vector<GLshort> texels(static_cast<std::size_t>(kEdge * kEdge) * 4u);
            for (std::size_t i = 0; i < texels.size(); i += 4) {
                texels[i] = 32767;
                texels[i + 1] = 0;
                texels[i + 2] = -32767;
                texels[i + 3] = 32767;
            }
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA16_SNORM, kEdge, kEdge);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kEdge, kEdge, GL_RGBA, GL_SHORT, texels.data());
            NearestWithMaxLevel(texture, 0);
            SyncBySampling(texture);
            const auto mark = PipeStatsWindow::MarkLaneLog();
            ExpectTexels("an RGBA16_SNORM immutable upload",
                         ImageCopy(texture, 0, GL_RGBA16_SNORM, "rgba16_snorm", true), kEdge,
                         Solid(kEdge, 255, 128, 0, 255), 3);
            ExpectFallbackNamedSince(mark);
        }

    } // namespace
} // namespace MGITest
