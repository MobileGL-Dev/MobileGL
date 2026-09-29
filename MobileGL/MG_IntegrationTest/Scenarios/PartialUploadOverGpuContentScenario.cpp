// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/PartialUploadOverGpuContentScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - A SMALL glTexSubImage INTO A LEVEL THE GPU HAS WRITTEN.
//
// Espryt keeps a CPU shadow of every level and syncs a pending client write by uploading the
// region it covers (SyncMipmapsToBackend's sub-rect path). The shadow never sees a GPU write, so
// only the region may go up: anything more puts the shadow's stale texels over what the GPU drew.
// The region upload used to be skipped whenever the shadow's bytes had to be CONVERTED on the way
// to the driver - a three-channel format the driver cannot render is stored four-channel and every
// upload is repacked - and whenever the ES upload is shaped differently from the shadow (a 1D
// array, stored as a 2D array one texel high): those levels went up WHOLE, and a 2x2 write erased
// the rest of the level.
//
// Every case uploads a level, lets the GPU clear it, writes one 2x2 box, and reads the level back:
// the clear outside the box, the write inside. The formats are picked by what they cost the ES
// upload, which is per driver - on Mesa llvmpipe GL_SRGB8, GL_RGB8_SNORM and GL_RGB8UI are stored
// four-channel (see ThreeChannelAttachmentScenario) while GL_RGB16F is native; ES has no renderable
// three-channel format at all, so on a phone all four are converted. GL_RGBA8 is the control that
// never converts. A format a backend cannot attach is skipped, not failed.
//
// The second half is about ORDER, with GL_RGBA8. Magma queues a clear until the texture's next
// use and records texel uploads into a batch submitted ahead of the frame's recorded commands, so
// a write issued after a clear of (or a draw into) the level reached the image FIRST and the
// earlier clear or draw landed over it: the box read back green. Each order is covered - the
// clear already recorded, still queued, synced by a draw or by the readback, the reverse order
// in which the clear must win, and a draw instead of a clear.
//
// The last part is about SCATTER: several boxes written between two syncs, with the GPU's clear in
// the gaps between them. Everything a backend has to go on is the union box and the storage's
// rect list, and both describe more than the writes - the box spans the gaps, the list merges
// touching rects into their bounding box, folds once it holds 96, and is withheld once its rects
// fill 3/4 of the box; a box spanning the level went up as the whole level. Espryt also uploaded
// the union box whenever its unpack ring was in use. Each case below reaches one of those: two
// distant boxes, a hundred of them, boxes filling more than 3/4 of their union, and boxes whose
// union spans the level.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <sstream>
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
        constexpr int kEdge = 8;
        constexpr int kBoxAt = 2;
        constexpr int kBoxEdge = 2;

        constexpr const char* kVS = R"(#version 430 core
void main() {
    vec2 positions[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    gl_Position = vec4(positions[gl_VertexID], 0.0, 1.0);
}
)";

        // How a value in [0, 1] is spelled in the client data, and how the level is cleared and
        // read back. Integer levels spell 1 as kIntegerOne.
        enum class Encoding { Unorm8, Snorm8, Float32, Uint8 };
        constexpr unsigned kIntegerOne = 9;

        struct Format {
            GLenum internalFormat;
            GLenum clientFormat;
            GLenum clientType;
            int components;
            Encoding encoding;
        };

        std::vector<std::uint8_t> Fill(const Format& f, int texels, float r, float g, float b) {
            const float rgba[4] = {r, g, b, 1.0f};
            const std::size_t componentBytes = f.encoding == Encoding::Float32 ? 4u : 1u;
            std::vector<std::uint8_t> out(static_cast<std::size_t>(texels) * f.components * componentBytes);
            for (int t = 0; t < texels; ++t) {
                for (int c = 0; c < f.components; ++c) {
                    std::uint8_t* dst = out.data() + (static_cast<std::size_t>(t) * f.components + c) * componentBytes;
                    const float v = rgba[c];
                    switch (f.encoding) {
                    case Encoding::Unorm8: *dst = static_cast<std::uint8_t>(std::lround(v * 255.0f)); break;
                    case Encoding::Snorm8:
                        *dst = static_cast<std::uint8_t>(static_cast<std::int8_t>(std::lround(v * 127.0f)));
                        break;
                    case Encoding::Float32: std::memcpy(dst, &v, sizeof(v)); break;
                    case Encoding::Uint8: *dst = static_cast<std::uint8_t>(std::lround(v * kIntegerOne)); break;
                    }
                }
            }
            return out;
        }

        class PartialUploadOverGpuContentScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                m_scratch = MakeColorFbo(4, 4);
                glGenVertexArrays(1, &m_vao);
                glGenFramebuffers(1, &m_fbo);
                FirstGLError();
            }

            void TearDown() override {
                if (!Ready()) return;
                glUseProgram(0);
                for (GLuint p : m_programs) glDeleteProgram(p);
                if (!m_textures.empty()) glDeleteTextures(static_cast<GLsizei>(m_textures.size()), m_textures.data());
                if (m_vao) glDeleteVertexArrays(1, &m_vao);
                if (m_fbo) glDeleteFramebuffers(1, &m_fbo);
                BindDefaultFramebuffer();
                DestroyColorFbo(m_scratch);
                glViewport(0, 0, Gl().Width(), Gl().Height());
                FirstGLError();
            }

            GLuint NewTexture(GLenum target) {
                GLuint t = 0;
                glGenTextures(1, &t);
                m_textures.push_back(t);
                glBindTexture(target, t);
                glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, 0);
                glBindTexture(target, 0);
                return t;
            }

            // A draw that samples the texture, which is what syncs its pending writes to the
            // driver (an FBO attach alone may find the framebuffer already synced).
            void SyncBySampling(GLenum target, GLuint tex, const char* samplerType, const char* coord) {
                std::string fs = std::string("#version 430 core\nuniform ") + samplerType +
                                 " u_tex;\nout vec4 o_color;\nvoid main() { o_color = vec4(texelFetch(u_tex, " + coord +
                                 ", 0)); }\n";
                std::string error;
                const GLuint program = CompileProgram(kVS, fs.c_str(), &error);
                ASSERT_NE(program, 0u) << error;
                m_programs.push_back(program);
                BindFbo(m_scratch);
                glUseProgram(program);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(target, tex);
                glUniform1i(glGetUniformLocation(program, "u_tex"), 0);
                glBindVertexArray(m_vao);
                glDrawArrays(GL_TRIANGLES, 0, 3);
                glBindVertexArray(0);
                glBindTexture(target, 0);
                glUseProgram(0);
                BindDefaultFramebuffer();
                glFinish();
                EXPECT_EQ(FirstGLError(), 0u) << "sampling sync";
            }

            // Attaches (tex, layer) to the scratch framebuffer; false when this backend cannot.
            bool Attach(GLuint tex, int layer) {
                glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
                if (layer >= 0) {
                    glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, tex, 0, layer);
                } else {
                    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
                }
                return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
            }

            void Detach() {
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
                BindDefaultFramebuffer();
            }

            // The GPU write the shadow never sees: the whole level (or layer) to green.
            void GpuClearGreen(const Format& f) {
                if (f.encoding == Encoding::Uint8) {
                    const GLuint green[4] = {0u, kIntegerOne, 0u, 1u};
                    glClearBufferuiv(GL_COLOR, 0, green);
                } else {
                    const GLfloat green[4] = {0.0f, 1.0f, 0.0f, 1.0f};
                    glClearBufferfv(GL_COLOR, 0, green);
                }
            }

            // The attached image's rgb, in units of "one", width x height texels.
            std::vector<float> ReadRgb(const Format& f, int width, int height) {
                std::vector<float> rgb(static_cast<std::size_t>(width * height) * 3u);
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                if (f.encoding == Encoding::Uint8) {
                    std::vector<GLuint> texels(static_cast<std::size_t>(width * height) * 4u, 0xA5A5A5A5u);
                    glReadPixels(0, 0, width, height, GL_RGBA_INTEGER, GL_UNSIGNED_INT, texels.data());
                    for (int i = 0; i < width * height; ++i)
                        for (int c = 0; c < 3; ++c)
                            rgb[i * 3 + c] = static_cast<float>(texels[i * 4 + c]) / kIntegerOne;
                } else {
                    std::vector<GLfloat> texels(static_cast<std::size_t>(width * height) * 4u, -7.0f);
                    glReadPixels(0, 0, width, height, GL_RGBA, GL_FLOAT, texels.data());
                    for (int i = 0; i < width * height; ++i)
                        for (int c = 0; c < 3; ++c) rgb[i * 3 + c] = texels[i * 4 + c];
                }
                EXPECT_EQ(FirstGLError(), 0u) << "readback";
                return rgb;
            }

            // Green everywhere but blue inside [boxX, boxX + boxW) x [boxY, boxY + boxH).
            static void ExpectClearAroundBox(const std::vector<float>& rgb, int width, int height, int boxX, int boxY,
                                             int boxW, int boxH) {
                int bad = 0;
                std::ostringstream first;
                for (int y = 0; y < height; ++y)
                    for (int x = 0; x < width; ++x) {
                        const bool inBox = x >= boxX && x < boxX + boxW && y >= boxY && y < boxY + boxH;
                        const float want[3] = {0.0f, inBox ? 0.0f : 1.0f, inBox ? 1.0f : 0.0f};
                        const float* got = &rgb[static_cast<std::size_t>(y * width + x) * 3u];
                        bool texelBad = false;
                        for (int c = 0; c < 3; ++c) texelBad |= std::fabs(got[c] - want[c]) > 0.02f;
                        if (texelBad && bad++ == 0) {
                            first << "(" << x << "," << y << ") got " << got[0] << "," << got[1] << "," << got[2]
                                  << " want " << want[0] << "," << want[1] << "," << want[2];
                        }
                    }
                EXPECT_EQ(bad, 0) << bad << " of " << width * height << " texels wrong; first " << first.str()
                                  << " (red outside the box: the stale shadow went back up; green inside it: the "
                                     "write landed before the clear or draw)";
            }

            // Upload red, sync, clear green on the GPU, sync, write a blue 2x2 box, sync, read.
            void Run2D(const Format& f) {
                const GLuint t = NewTexture(GL_TEXTURE_2D);
                const bool integer = f.encoding == Encoding::Uint8;
                const char* sampler = integer ? "usampler2D" : "sampler2D";
                const auto red = Fill(f, kEdge * kEdge, 1.0f, 0.0f, 0.0f);
                glBindTexture(GL_TEXTURE_2D, t);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(f.internalFormat), kEdge, kEdge, 0, f.clientFormat,
                             f.clientType, red.data());
                glBindTexture(GL_TEXTURE_2D, 0);
                ASSERT_EQ(FirstGLError(), 0u) << "definition";
                SyncBySampling(GL_TEXTURE_2D, t, sampler, "ivec2(0)");
                if (!Attach(t, -1)) {
                    Detach();
                    GTEST_SKIP() << "the format does not attach on backend " << Gl().BackendName();
                }
                GpuClearGreen(f);
                Detach();
                SyncBySampling(GL_TEXTURE_2D, t, sampler, "ivec2(0)");
                const auto blue = Fill(f, kBoxEdge * kBoxEdge, 0.0f, 0.0f, 1.0f);
                glBindTexture(GL_TEXTURE_2D, t);
                glTexSubImage2D(GL_TEXTURE_2D, 0, kBoxAt, kBoxAt, kBoxEdge, kBoxEdge, f.clientFormat, f.clientType,
                                blue.data());
                glBindTexture(GL_TEXTURE_2D, 0);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
                SyncBySampling(GL_TEXTURE_2D, t, sampler, "ivec2(0)");
                ASSERT_TRUE(Attach(t, -1));
                const auto rgb = ReadRgb(f, kEdge, kEdge);
                Detach();
                ExpectClearAroundBox(rgb, kEdge, kEdge, kBoxAt, kBoxAt, kBoxEdge, kBoxEdge);
            }

            // P11 M: TWO SCATTERED WRITES INTO ONE LEVEL, so the level carries a rect list (their
            // area is well under 3/4 of their union box) and not just a box. Under a transport
            // (and on the push builds' monolith arm) the list crosses in the client's record,
            // whose regions state their source pitch and offset in the CLIENT's layout; a level
            // whose upload is converted is cut from the conversion's wider buffer, so those
            // numbers must not address it (Managers.cpp, carriedLayoutApplies). The whole level
            // is uploaded green first, so every texel the union box spans outside the two boxes
            // is green in the shadow too: this case is about WHERE the boxes land, not about GPU
            // content between them. Green everywhere but the two blue boxes.
            void Run2DScatter(const Format& f) {
                constexpr int kFirstAt = 1, kSecondAt = 5;
                const GLuint t = NewTexture(GL_TEXTURE_2D);
                const bool integer = f.encoding == Encoding::Uint8;
                const char* sampler = integer ? "usampler2D" : "sampler2D";
                const auto green = Fill(f, kEdge * kEdge, 0.0f, 1.0f, 0.0f);
                glBindTexture(GL_TEXTURE_2D, t);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(f.internalFormat), kEdge, kEdge, 0, f.clientFormat,
                             f.clientType, green.data());
                glBindTexture(GL_TEXTURE_2D, 0);
                ASSERT_EQ(FirstGLError(), 0u) << "definition";
                SyncBySampling(GL_TEXTURE_2D, t, sampler, "ivec2(0)");
                if (!Attach(t, -1)) {
                    Detach();
                    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
                    GTEST_SKIP() << "the format does not attach on backend " << Gl().BackendName();
                }
                Detach();
                const auto blue = Fill(f, kBoxEdge * kBoxEdge, 0.0f, 0.0f, 1.0f);
                glBindTexture(GL_TEXTURE_2D, t);
                glTexSubImage2D(GL_TEXTURE_2D, 0, kFirstAt, kFirstAt, kBoxEdge, kBoxEdge, f.clientFormat, f.clientType,
                                blue.data());
                glTexSubImage2D(GL_TEXTURE_2D, 0, kSecondAt, kSecondAt, kBoxEdge, kBoxEdge, f.clientFormat,
                                f.clientType, blue.data());
                glBindTexture(GL_TEXTURE_2D, 0);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
                SyncBySampling(GL_TEXTURE_2D, t, sampler, "ivec2(0)");
                ASSERT_TRUE(Attach(t, -1));
                const auto rgb = ReadRgb(f, kEdge, kEdge);
                Detach();
                int bad = 0;
                std::ostringstream first;
                for (int y = 0; y < kEdge; ++y)
                    for (int x = 0; x < kEdge; ++x) {
                        const auto inBox = [&](int at) {
                            return x >= at && x < at + kBoxEdge && y >= at && y < at + kBoxEdge;
                        };
                        const bool blueHere = inBox(kFirstAt) || inBox(kSecondAt);
                        const float want[3] = {0.0f, blueHere ? 0.0f : 1.0f, blueHere ? 1.0f : 0.0f};
                        const float* got = &rgb[static_cast<std::size_t>(y * kEdge + x) * 3u];
                        bool texelBad = false;
                        for (int c = 0; c < 3; ++c) texelBad |= std::fabs(got[c] - want[c]) > 0.02f;
                        if (texelBad && bad++ == 0) {
                            first << "(" << x << "," << y << ") got " << got[0] << "," << got[1] << "," << got[2]
                                  << " want " << want[0] << "," << want[1] << "," << want[2];
                        }
                    }
                EXPECT_EQ(bad, 0) << bad << " of " << kEdge * kEdge << " texels wrong; first " << first.str()
                                  << " (a box read at the wrong pitch or offset lands the wrong bytes)";
            }

            // Building blocks of the RGBA8 ordering cases: the same texels, issued in the orders
            // a backend that defers work may get wrong.
            static Format Rgba8() { return {GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 4, Encoding::Unorm8}; }

            GLuint NewRedRgba8() {
                const GLuint t = NewTexture(GL_TEXTURE_2D);
                const auto red = Fill(Rgba8(), kEdge * kEdge, 1.0f, 0.0f, 0.0f);
                glBindTexture(GL_TEXTURE_2D, t);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kEdge, kEdge, 0, GL_RGBA, GL_UNSIGNED_BYTE, red.data());
                glBindTexture(GL_TEXTURE_2D, 0);
                EXPECT_EQ(FirstGLError(), 0u) << "definition";
                return t;
            }

            void SampleRgba8(GLuint t) { SyncBySampling(GL_TEXTURE_2D, t, "sampler2D", "ivec2(0)"); }

            void ClearRgba8Green(GLuint t) {
                ASSERT_TRUE(Attach(t, -1));
                GpuClearGreen(Rgba8());
                Detach();
            }

            // A draw that writes green over the whole level.
            void DrawRgba8Green(GLuint t) {
                std::string error;
                const GLuint program = CompileProgram(
                    kVS, "#version 430 core\nout vec4 o_color;\nvoid main() { o_color = vec4(0.0, 1.0, 0.0, 1.0); }\n",
                    &error);
                ASSERT_NE(program, 0u) << error;
                m_programs.push_back(program);
                ASSERT_TRUE(Attach(t, -1));
                glViewport(0, 0, kEdge, kEdge);
                glUseProgram(program);
                glBindVertexArray(m_vao);
                glDrawArrays(GL_TRIANGLES, 0, 3);
                glBindVertexArray(0);
                glUseProgram(0);
                Detach();
                glViewport(0, 0, Gl().Width(), Gl().Height());
            }

            void WriteRgba8BlueBox(GLuint t) {
                const auto blue = Fill(Rgba8(), kBoxEdge * kBoxEdge, 0.0f, 0.0f, 1.0f);
                glBindTexture(GL_TEXTURE_2D, t);
                glTexSubImage2D(GL_TEXTURE_2D, 0, kBoxAt, kBoxAt, kBoxEdge, kBoxEdge, GL_RGBA, GL_UNSIGNED_BYTE,
                                blue.data());
                glBindTexture(GL_TEXTURE_2D, 0);
            }

            // Green everywhere, and blue in the box when `boxWins`.
            void ExpectRgba8(GLuint t, bool boxWins) {
                ASSERT_TRUE(Attach(t, -1));
                const auto rgb = ReadRgb(Rgba8(), kEdge, kEdge);
                Detach();
                ExpectClearAroundBox(rgb, kEdge, kEdge, kBoxAt, kBoxAt, boxWins ? kBoxEdge : 0, boxWins ? kBoxEdge : 0);
            }

            struct Box {
                int x, y, w, h;
            };

            // An edge x edge RGBA8 level uploaded red, cleared green on the GPU, then written blue in
            // every box by one glTexSubImage each, with no sync in between; read back after a sync.
            // Blue inside the boxes, green everywhere else.
            void RunScatteredWrites(int edge, const std::vector<Box>& boxes) {
                const GLuint t = NewTexture(GL_TEXTURE_2D);
                const auto red = Fill(Rgba8(), edge * edge, 1.0f, 0.0f, 0.0f);
                glBindTexture(GL_TEXTURE_2D, t);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, edge, edge, 0, GL_RGBA, GL_UNSIGNED_BYTE, red.data());
                glBindTexture(GL_TEXTURE_2D, 0);
                ASSERT_EQ(FirstGLError(), 0u) << "definition";
                SampleRgba8(t);
                ClearRgba8Green(t);
                SampleRgba8(t);
                glBindTexture(GL_TEXTURE_2D, t);
                for (const Box& box : boxes) {
                    const auto blue = Fill(Rgba8(), box.w * box.h, 0.0f, 0.0f, 1.0f);
                    glTexSubImage2D(GL_TEXTURE_2D, 0, box.x, box.y, box.w, box.h, GL_RGBA, GL_UNSIGNED_BYTE,
                                    blue.data());
                }
                glBindTexture(GL_TEXTURE_2D, 0);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
                ASSERT_EQ(FirstGLError(), 0u) << "writes";
                SampleRgba8(t);
                ASSERT_TRUE(Attach(t, -1));
                const auto rgb = ReadRgb(Rgba8(), edge, edge);
                Detach();
                int bad = 0, stale = 0;
                std::ostringstream first;
                for (int y = 0; y < edge; ++y)
                    for (int x = 0; x < edge; ++x) {
                        bool inBox = false;
                        for (const Box& box : boxes)
                            inBox |= x >= box.x && x < box.x + box.w && y >= box.y && y < box.y + box.h;
                        const float want[3] = {0.0f, inBox ? 0.0f : 1.0f, inBox ? 1.0f : 0.0f};
                        const float* got = &rgb[static_cast<std::size_t>(y * edge + x) * 3u];
                        bool texelBad = false;
                        for (int c = 0; c < 3; ++c) texelBad |= std::fabs(got[c] - want[c]) > 0.02f;
                        if (!texelBad) continue;
                        stale += got[0] > 0.5f;
                        if (bad++ == 0) {
                            first << "(" << x << "," << y << ") got " << got[0] << "," << got[1] << "," << got[2]
                                  << " want " << want[0] << "," << want[1] << "," << want[2];
                        }
                    }
                EXPECT_EQ(bad, 0) << bad << " of " << edge * edge << " texels wrong, " << stale
                                  << " of them red - the stale shadow went up outside the writes; first "
                                  << first.str();
            }

            GLuint m_vao = 0, m_fbo = 0;
            ColorFbo m_scratch{};
            std::vector<GLuint> m_programs;
            std::vector<GLuint> m_textures;
        };

        // Control: the shadow's bytes go to the driver as they are.
        TEST_F(PartialUploadOverGpuContentScenario, Rgba8SubImageKeepsTheGpuClear) {
            if (!Ready()) return;
            Run2D({GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 4, Encoding::Unorm8});
        }

        TEST_F(PartialUploadOverGpuContentScenario, Srgb8SubImageKeepsTheGpuClear) {
            if (!Ready()) return;
            Run2D({GL_SRGB8, GL_RGB, GL_UNSIGNED_BYTE, 3, Encoding::Unorm8});
        }

        TEST_F(PartialUploadOverGpuContentScenario, Rgb8SnormSubImageKeepsTheGpuClear) {
            if (!Ready()) return;
            Run2D({GL_RGB8_SNORM, GL_RGB, GL_BYTE, 3, Encoding::Snorm8});
        }

        TEST_F(PartialUploadOverGpuContentScenario, Rgb16fSubImageKeepsTheGpuClear) {
            if (!Ready()) return;
            Run2D({GL_RGB16F, GL_RGB, GL_FLOAT, 3, Encoding::Float32});
        }

        TEST_F(PartialUploadOverGpuContentScenario, Rgb8uiSubImageKeepsTheGpuClear) {
            if (!Ready()) return;
            Run2D({GL_RGB8UI, GL_RGB_INTEGER, GL_UNSIGNED_BYTE, 3, Encoding::Uint8});
        }

        // P11 M: the rect-list shape of the same upload (Run2DScatter). RGBA8 is the control whose
        // record pitch does address the uploaded bytes; the other two are stored four-channel on
        // llvmpipe and on every ES driver, so their boxes are cut from the conversion's buffer.
        TEST_F(PartialUploadOverGpuContentScenario, Rgba8ScatteredSubImagesLandWhereWritten) {
            if (!Ready()) return;
            Run2DScatter({GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 4, Encoding::Unorm8});
        }

        TEST_F(PartialUploadOverGpuContentScenario, Srgb8ScatteredSubImagesLandWhereWritten) {
            if (!Ready()) return;
            Run2DScatter({GL_SRGB8, GL_RGB, GL_UNSIGNED_BYTE, 3, Encoding::Unorm8});
        }

        TEST_F(PartialUploadOverGpuContentScenario, Rgb8uiScatteredSubImagesLandWhereWritten) {
            if (!Ready()) return;
            Run2DScatter({GL_RGB8UI, GL_RGB_INTEGER, GL_UNSIGNED_BYTE, 3, Encoding::Uint8});
        }

        // A 1D array's ES image is a 2D array one texel high with the layers in depth, so the
        // upload is shaped differently from the shadow (layers as rows) even with no conversion.
        TEST_F(PartialUploadOverGpuContentScenario, OneDimensionalArraySubImageKeepsTheGpuClearedLayer) {
            if (!Ready()) return;
            constexpr int kLayers = 2;
            const Format f{GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 4, Encoding::Unorm8};
            const GLuint t = NewTexture(GL_TEXTURE_1D_ARRAY);
            const auto red = Fill(f, kEdge * kLayers, 1.0f, 0.0f, 0.0f);
            glBindTexture(GL_TEXTURE_1D_ARRAY, t);
            glTexImage2D(GL_TEXTURE_1D_ARRAY, 0, GL_RGBA8, kEdge, kLayers, 0, GL_RGBA, GL_UNSIGNED_BYTE, red.data());
            glBindTexture(GL_TEXTURE_1D_ARRAY, 0);
            ASSERT_EQ(FirstGLError(), 0u) << "definition";
            SyncBySampling(GL_TEXTURE_1D_ARRAY, t, "sampler1DArray", "ivec2(0)");
            if (!Attach(t, 1)) {
                Detach();
                GTEST_SKIP() << "a 1D array layer does not attach on backend " << Gl().BackendName();
            }
            GpuClearGreen(f);
            Detach();
            SyncBySampling(GL_TEXTURE_1D_ARRAY, t, "sampler1DArray", "ivec2(0)");
            const auto blue = Fill(f, kBoxEdge, 0.0f, 0.0f, 1.0f);
            glBindTexture(GL_TEXTURE_1D_ARRAY, t);
            glTexSubImage2D(GL_TEXTURE_1D_ARRAY, 0, kBoxAt, 1, kBoxEdge, 1, GL_RGBA, GL_UNSIGNED_BYTE, blue.data());
            glBindTexture(GL_TEXTURE_1D_ARRAY, 0);
            SyncBySampling(GL_TEXTURE_1D_ARRAY, t, "sampler1DArray", "ivec2(0)");
            ASSERT_TRUE(Attach(t, 1));
            const auto layer1 = ReadRgb(f, kEdge, 1);
            Detach();
            ExpectClearAroundBox(layer1, kEdge, 1, kBoxAt, 0, kBoxEdge, 1);
        }

        // The orders below matter to a backend that defers GPU work (Magma queues clears and runs
        // texel uploads in a batch ahead of the frame's recorded commands); every one is plain GL.

        // The clear is still queued when the write arrives: it has to reach the level first.
        TEST_F(PartialUploadOverGpuContentScenario, SubImageAfterAQueuedClearLandsOnTheClear) {
            if (!Ready()) return;
            const GLuint t = NewRedRgba8();
            SampleRgba8(t);
            ClearRgba8Green(t);
            WriteRgba8BlueBox(t);
            SampleRgba8(t);
            ExpectRgba8(t, true);
        }

        // Same, with the readback as the first thing to sync the write.
        TEST_F(PartialUploadOverGpuContentScenario, SubImageAfterAQueuedClearLandsOnTheClearForAReadback) {
            if (!Ready()) return;
            const GLuint t = NewRedRgba8();
            SampleRgba8(t);
            ClearRgba8Green(t);
            WriteRgba8BlueBox(t);
            ExpectRgba8(t, true);
        }

        // The other order: the write is still pending when the clear arrives, and the clear wins.
        TEST_F(PartialUploadOverGpuContentScenario, ClearAfterAPendingSubImageWins) {
            if (!Ready()) return;
            const GLuint t = NewRedRgba8();
            SampleRgba8(t);
            WriteRgba8BlueBox(t);
            ClearRgba8Green(t);
            SampleRgba8(t);
            ExpectRgba8(t, false);
        }

        // A draw into the level instead of a clear.
        TEST_F(PartialUploadOverGpuContentScenario, SubImageAfterADrawIntoTheLevelLandsOnTheDraw) {
            if (!Ready()) return;
            const GLuint t = NewRedRgba8();
            SampleRgba8(t);
            DrawRgba8Green(t);
            WriteRgba8BlueBox(t);
            SampleRgba8(t);
            ExpectRgba8(t, true);
        }

        // Two boxes in opposite corners: their union box is 7x7, of which the writes are 8 texels.
        TEST_F(PartialUploadOverGpuContentScenario, TwoDistantSubImagesKeepTheGpuClearBetweenThem) {
            if (!Ready()) return;
            RunScatteredWrites(kEdge, {{0, 0, 2, 2}, {5, 5, 2, 2}});
        }

        // A hundred 2x2 boxes two texels apart: more than the 96 rects the storage keeps, so its
        // list folds neighbours together, gaps and all.
        TEST_F(PartialUploadOverGpuContentScenario, AHundredSubImagesKeepTheGpuClearBetweenThem) {
            if (!Ready()) return;
            std::vector<Box> boxes;
            for (int j = 0; j < 10; ++j)
                for (int i = 0; i < 10; ++i) boxes.push_back({4 * i + 1, 4 * j + 1, 2, 2});
            RunScatteredWrites(40, boxes);
        }

        // Four 4x4 boxes around a one-texel cross: 64 of their union's 81 texels, past the 3/4 at
        // which the storage stops handing its rect list out.
        TEST_F(PartialUploadOverGpuContentScenario, SubImagesFillingThreeQuartersOfTheirUnionKeepTheGpuClearBetweenThem) {
            if (!Ready()) return;
            RunScatteredWrites(16, {{2, 2, 4, 4}, {7, 2, 4, 4}, {2, 7, 4, 4}, {7, 7, 4, 4}});
        }

        // Three quadrants: the union box is the whole level, and the list merges the touching
        // boxes into it; the fourth quadrant is still the GPU's.
        TEST_F(PartialUploadOverGpuContentScenario, SubImagesWhoseUnionSpansTheLevelKeepTheGpuClearInTheGap) {
            if (!Ready()) return;
            RunScatteredWrites(kEdge, {{0, 0, 4, 4}, {4, 0, 4, 4}, {0, 4, 4, 4}});
        }
    } // namespace
} // namespace MGITest
