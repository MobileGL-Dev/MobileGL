// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/ImageBindableRemintScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - THE FIRST glBindImageTexture OF A TEXTURE THE GPU HAS ALREADY WRITTEN.
//
// ES only allows immutable storage on an image unit, and a few formats need a wider carrier on
// top (TextureImpl::GetImageBindableStorageWidening), so Espryt re-mints a texture the first time
// it is image-bound after it already has driver storage (RequireImageBindableStorage). The new
// storage has to hold what the old one held. The re-mint used to fill it from the frontend's
// shadow - the CLIENT's copy of each level - and the shadow never sees a GPU write: a render or
// clear into the level, a glGenerateMipmap, a null-data definition only the GPU ever filled. Every
// such level came back as the bytes the application last uploaded, or zeroes. That is the
// ~26.9k-pixel iris-derivative difference: an RGBA16F render target, first image-bound after it
// was rendered into.
//
// Every case gives a texture its content by one means, lets the backend create its storage (a
// sampling draw, or the FBO attach of a GPU clear), writes it on the GPU where the case says so,
// and then binds it to an image unit for the first time: a compute imageLoad copies the level into
// a destination that was image-bound before it was ever synced (so the destination itself never
// re-mints), and the destination is read back through an FBO. The expectation is the GL answer.
//
// The controls are the other half: levels the GPU never wrote (including formats whose driver
// readback may be refused), levels the driver does not have yet because they were defined after
// the last sync, and a pending client write that has to land on top of GPU content. Magma never
// re-mints from a shadow (it copies image to image) and is the control backend.

#include <array>
#include <cstdint>
#include <cstdlib>
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

        constexpr const char* kVS = R"(#version 430 core
void main() {
    vec2 positions[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    gl_Position = vec4(positions[gl_VertexID], 0.0, 1.0);
}
)";
        constexpr const char* kSampleFS = R"(#version 430 core
uniform sampler2D u_tex;
out vec4 o_color;
void main() { o_color = texelFetch(u_tex, ivec2(gl_FragCoord.xy) % 4, 0); }
)";

        // Copies an image into an RGBA8 image with imageLoad; a signed-normalized source is mapped
        // to [0, 1] so it survives the unsigned destination.
        std::string CopyCS(const char* srcFormat, bool snorm) {
            std::string s = "#version 430 core\nlayout(local_size_x = 8, local_size_y = 8) in;\n";
            s += std::string("layout(") + srcFormat + ", binding = 0) readonly uniform image2D u_src;\n";
            s += "layout(rgba8, binding = 1) writeonly uniform image2D u_dst;\n";
            s += "void main() { ivec2 p = ivec2(gl_GlobalInvocationID.xy); vec4 v = imageLoad(u_src, p);\n";
            if (snorm) s += "  v = v * 0.5 + 0.5;\n";
            s += "  imageStore(u_dst, p, v); }\n";
            return s;
        }

        std::vector<GLubyte> Pattern(int edge, GLubyte magic) {
            std::vector<GLubyte> t(static_cast<std::size_t>(edge * edge) * 4u);
            for (int y = 0; y < edge; ++y)
                for (int x = 0; x < edge; ++x) {
                    const std::size_t at = static_cast<std::size_t>((y * edge + x) * 4);
                    t[at + 0] = magic;
                    t[at + 1] = static_cast<GLubyte>(16 * (x + 1));
                    t[at + 2] = static_cast<GLubyte>(16 * (y + 1));
                    t[at + 3] = 0xFF;
                }
            return t;
        }

        std::vector<GLubyte> Solid(int edge, GLubyte r, GLubyte g, GLubyte b, GLubyte a) {
            std::vector<GLubyte> t(static_cast<std::size_t>(edge * edge) * 4u);
            for (std::size_t i = 0; i < t.size(); i += 4) {
                t[i] = r;
                t[i + 1] = g;
                t[i + 2] = b;
                t[i + 3] = a;
            }
            return t;
        }

        const GLfloat kGreen[4] = {0.0f, 1.0f, 0.0f, 1.0f};

        class ImageBindableRemintScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                std::string error;
                m_sampleProgram = CompileProgram(kVS, kSampleFS, &error);
                ASSERT_NE(m_sampleProgram, 0u) << error;
                m_scratch = MakeColorFbo(4, 4);
                glGenVertexArrays(1, &m_vao);
                FirstGLError();
            }

            void TearDown() override {
                if (!Ready()) return;
                glUseProgram(0);
                glBindImageTexture(0, 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);
                glBindImageTexture(1, 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);
                if (m_sampleProgram) glDeleteProgram(m_sampleProgram);
                if (m_vao) glDeleteVertexArrays(1, &m_vao);
                if (!m_textures.empty()) glDeleteTextures(static_cast<GLsizei>(m_textures.size()), m_textures.data());
                if (m_fbo) glDeleteFramebuffers(1, &m_fbo);
                BindDefaultFramebuffer();
                DestroyColorFbo(m_scratch);
                glViewport(0, 0, Gl().Width(), Gl().Height());
                FirstGLError();
            }

            GLuint NewTexture() {
                GLuint t = 0;
                glGenTextures(1, &t);
                m_textures.push_back(t);
                return t;
            }

            static void Nearest(GLenum target, GLuint tex, int maxLevel) {
                glBindTexture(target, tex);
                glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, maxLevel);
                glBindTexture(target, 0);
            }

            // A draw that samples the texture: the backend now holds storage for it.
            void SyncBySampling(GLuint tex) {
                BindFbo(m_scratch);
                glUseProgram(m_sampleProgram);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, tex);
                glUniform1i(glGetUniformLocation(m_sampleProgram, "u_tex"), 0);
                glBindVertexArray(m_vao);
                glDrawArrays(GL_TRIANGLES, 0, 3);
                glBindVertexArray(0);
                glBindTexture(GL_TEXTURE_2D, 0);
                glUseProgram(0);
                BindDefaultFramebuffer();
                glFinish();
            }

            // A GPU write the frontend's shadow never sees. A layer >= 0 names one layer of an
            // array texture.
            void GpuClear(GLuint tex, int level, const GLfloat* color, int layer = -1) {
                if (!m_fbo) glGenFramebuffers(1, &m_fbo);
                glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
                if (layer >= 0) {
                    glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, tex, level, layer);
                } else {
                    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, level);
                }
                EXPECT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GLenum(GL_FRAMEBUFFER_COMPLETE))
                    << "the clear target is incomplete";
                glClearBufferfv(GL_COLOR, 0, color);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
                BindDefaultFramebuffer();
            }

            // The first image binding of (src, level[, layer]): an imageLoad copy into a fresh RGBA8
            // destination, read back through an FBO.
            std::vector<GLubyte> ImageCopy(GLuint src, int level, GLenum unitFormat, const char* glslFormat,
                                           bool snorm = false, int layer = 0) {
                const GLuint dst = NewTexture();
                glBindTexture(GL_TEXTURE_2D, dst);
                glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, kEdge, kEdge);
                glBindTexture(GL_TEXTURE_2D, 0);
                glBindImageTexture(1, dst, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
                glBindImageTexture(0, src, level, GL_FALSE, layer, GL_READ_ONLY, unitFormat);
                EXPECT_EQ(FirstGLError(), 0u) << "glBindImageTexture";
                const std::string cs = CopyCS(glslFormat, snorm);
                const char* csp = cs.c_str();
                const GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
                glShaderSource(shader, 1, &csp, nullptr);
                glCompileShader(shader);
                const GLuint program = glCreateProgram();
                glAttachShader(program, shader);
                glLinkProgram(program);
                glDeleteShader(shader);
                GLint linked = GL_FALSE;
                glGetProgramiv(program, GL_LINK_STATUS, &linked);
                EXPECT_EQ(linked, GL_TRUE) << "the copy program did not link";
                glUseProgram(program);
                // P11 M: A BARRIER THE SPEC DOES NOT ASK FOR, pinned for the reason
                // TextureRemintPullScenario's twin gives (ID-P9-11, DEBTS "待归因"): on llvmpipe the
                // kept-allocation case (GpuClearOverAnImmutableUploadSurvives - no re-mint to
                // serialize anything) read the pre-clear upload once on the push-monolith arm under
                // load, the same shape P9 measured 3/30 on the split arm. The cases ask what the
                // storage HOLDS after the first image binding, not how a clear orders against an
                // image load, so the ordering is pinned here.
                glMemoryBarrier(GL_ALL_BARRIER_BITS);
                glDispatchCompute(1, 1, 1);
                glMemoryBarrier(GL_ALL_BARRIER_BITS);
                glUseProgram(0);
                glDeleteProgram(program);
                glBindImageTexture(0, 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);
                glBindImageTexture(1, 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);
                if (!m_fbo) glGenFramebuffers(1, &m_fbo);
                glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst, 0);
                std::vector<GLubyte> out(static_cast<std::size_t>(kEdge * kEdge) * 4u, 0xA5);
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glReadPixels(0, 0, kEdge, kEdge, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
                BindDefaultFramebuffer();
                EXPECT_EQ(FirstGLError(), 0u) << "copy/readback";
                return out;
            }

            // The edge x edge corner of `got` against an RGBA8 expectation, the first `channels`
            // channels only, within 2 codes (a normalized value's round trip).
            static void ExpectImage(const std::vector<GLubyte>& got, int edge, const std::vector<GLubyte>& want,
                                    int channels = 4) {
                int bad = 0;
                std::ostringstream first;
                for (int y = 0; y < edge; ++y)
                    for (int x = 0; x < edge; ++x) {
                        const std::size_t g = static_cast<std::size_t>((y * kEdge + x) * 4);
                        const std::size_t w = static_cast<std::size_t>((y * edge + x) * 4);
                        bool texelBad = false;
                        for (int c = 0; c < channels; ++c)
                            texelBad |= std::abs(int(got[g + c]) - int(want[w + c])) > 2;
                        if (texelBad && bad++ == 0) {
                            first << "(" << x << "," << y << ") got " << int(got[g]) << "," << int(got[g + 1]) << ","
                                  << int(got[g + 2]) << "," << int(got[g + 3]) << " want " << int(want[w]) << ","
                                  << int(want[w + 1]) << "," << int(want[w + 2]) << "," << int(want[w + 3]);
                        }
                    }
                EXPECT_EQ(bad, 0) << bad << " of " << edge * edge << " texels differ; first " << first.str();
            }

            // One glTexSubImage of opaque blue per {x, y, w, h} box, left pending.
            static void WriteBlueBoxes(GLuint tex, const std::vector<std::array<int, 4>>& boxes) {
                glBindTexture(GL_TEXTURE_2D, tex);
                for (const auto& b : boxes) {
                    std::vector<GLubyte> blue(static_cast<std::size_t>(b[2] * b[3]) * 4u, 0);
                    for (std::size_t i = 0; i < blue.size(); i += 4) {
                        blue[i + 2] = 255;
                        blue[i + 3] = 255;
                    }
                    glTexSubImage2D(GL_TEXTURE_2D, 0, b[0], b[1], b[2], b[3], GL_RGBA, GL_UNSIGNED_BYTE, blue.data());
                }
                glBindTexture(GL_TEXTURE_2D, 0);
            }

            // kEdge x kEdge green, blue inside the boxes.
            static std::vector<GLubyte> GreenWithBlueBoxes(const std::vector<std::array<int, 4>>& boxes) {
                auto want = Solid(kEdge, 0, 255, 0, 255);
                for (const auto& b : boxes)
                    for (int y = b[1]; y < b[1] + b[3]; ++y)
                        for (int x = b[0]; x < b[0] + b[2]; ++x) {
                            const std::size_t at = static_cast<std::size_t>((y * kEdge + x) * 4);
                            want[at] = 0;
                            want[at + 1] = 0;
                            want[at + 2] = 255;
                            want[at + 3] = 255;
                        }
                return want;
            }

            GLuint m_sampleProgram = 0, m_vao = 0, m_fbo = 0;
            ColorFbo m_scratch{};
            std::vector<GLuint> m_textures;
        };

        // Control: the shadow and the driver agree.
        TEST_F(ImageBindableRemintScenario, UploadedLevelKeepsItsUpload) {
            if (!Ready()) return;
            const GLuint t = NewTexture();
            const auto p = Pattern(kEdge, 0x40);
            glBindTexture(GL_TEXTURE_2D, t);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kEdge, kEdge, 0, GL_RGBA, GL_UNSIGNED_BYTE, p.data());
            Nearest(GL_TEXTURE_2D, t, 0);
            SyncBySampling(t);
            ExpectImage(ImageCopy(t, 0, GL_RGBA8, "rgba8"), kEdge, p);
        }

        TEST_F(ImageBindableRemintScenario, GpuClearOverAnUploadSurvives) {
            if (!Ready()) return;
            const GLuint t = NewTexture();
            const auto p = Pattern(kEdge, 0x40);
            glBindTexture(GL_TEXTURE_2D, t);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kEdge, kEdge, 0, GL_RGBA, GL_UNSIGNED_BYTE, p.data());
            Nearest(GL_TEXTURE_2D, t, 0);
            SyncBySampling(t);
            GpuClear(t, 0, kGreen);
            SyncBySampling(t);
            ExpectImage(ImageCopy(t, 0, GL_RGBA8, "rgba8"), kEdge, Solid(kEdge, 0, 255, 0, 255));
        }

        // Immutable in a core image format: the storage is already what an image unit needs.
        TEST_F(ImageBindableRemintScenario, GpuClearOverAnImmutableUploadSurvives) {
            if (!Ready()) return;
            const GLuint t = NewTexture();
            const auto p = Pattern(kEdge, 0x40);
            glBindTexture(GL_TEXTURE_2D, t);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, kEdge, kEdge);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kEdge, kEdge, GL_RGBA, GL_UNSIGNED_BYTE, p.data());
            Nearest(GL_TEXTURE_2D, t, 0);
            SyncBySampling(t);
            GpuClear(t, 0, kGreen);
            SyncBySampling(t);
            ExpectImage(ImageCopy(t, 0, GL_RGBA8, "rgba8"), kEdge, Solid(kEdge, 0, 255, 0, 255));
        }

        // A null-data definition only the GPU ever filled: the shadow holds zeroes.
        TEST_F(ImageBindableRemintScenario, GpuClearOfANullDefinitionSurvives) {
            if (!Ready()) return;
            const GLuint t = NewTexture();
            glBindTexture(GL_TEXTURE_2D, t);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kEdge, kEdge, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            Nearest(GL_TEXTURE_2D, t, 0);
            SyncBySampling(t);
            GpuClear(t, 0, kGreen);
            SyncBySampling(t);
            ExpectImage(ImageCopy(t, 0, GL_RGBA8, "rgba8"), kEdge, Solid(kEdge, 0, 255, 0, 255));
        }

        // A client write still pending when the bind comes lands on top of the GPU's content,
        // and only inside its own box.
        TEST_F(ImageBindableRemintScenario, PendingSubImageLandsOnTheGpuClear) {
            if (!Ready()) return;
            const GLuint t = NewTexture();
            const auto p = Pattern(kEdge, 0x40);
            glBindTexture(GL_TEXTURE_2D, t);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kEdge, kEdge, 0, GL_RGBA, GL_UNSIGNED_BYTE, p.data());
            Nearest(GL_TEXTURE_2D, t, 0);
            SyncBySampling(t);
            GpuClear(t, 0, kGreen);
            SyncBySampling(t);
            const auto blue = Solid(2, 0, 0, 255, 255);
            glBindTexture(GL_TEXTURE_2D, t);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 2, 2, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, blue.data());
            glBindTexture(GL_TEXTURE_2D, 0);
            auto want = Solid(kEdge, 0, 255, 0, 255);
            for (int y = 2; y < 4; ++y)
                for (int x = 2; x < 4; ++x) {
                    const std::size_t at = static_cast<std::size_t>((y * kEdge + x) * 4);
                    want[at] = 0;
                    want[at + 1] = 0;
                    want[at + 2] = 255;
                    want[at + 3] = 255;
                }
            ExpectImage(ImageCopy(t, 0, GL_RGBA8, "rgba8"), kEdge, want);
        }

        // Several pending writes: only their texels land on the GPU's clear, never the gaps between
        // them. The re-mint merged the union box of the shadow over the readback whenever the rect
        // list was withheld, and kept the shadow outright when that box spanned the level.

        // Two 3x7 columns with a one-texel gap: 42 of their union's 49 texels, past the 3/4 at which
        // the storage withholds its rect list.
        TEST_F(ImageBindableRemintScenario, PendingSubImagesFillingThreeQuartersOfTheirUnionLandOnTheGpuClear) {
            if (!Ready()) return;
            const std::vector<std::array<int, 4>> boxes = {{0, 0, 3, 7}, {4, 0, 3, 7}};
            const GLuint t = NewTexture();
            const auto p = Pattern(kEdge, 0x40);
            glBindTexture(GL_TEXTURE_2D, t);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kEdge, kEdge, 0, GL_RGBA, GL_UNSIGNED_BYTE, p.data());
            Nearest(GL_TEXTURE_2D, t, 0);
            SyncBySampling(t);
            GpuClear(t, 0, kGreen);
            SyncBySampling(t);
            WriteBlueBoxes(t, boxes);
            ExpectImage(ImageCopy(t, 0, GL_RGBA8, "rgba8"), kEdge, GreenWithBlueBoxes(boxes));
        }

        // Three quadrants: the union box is the whole level; the fourth quadrant is the GPU's.
        TEST_F(ImageBindableRemintScenario, PendingSubImagesWhoseUnionSpansTheLevelLandOnTheGpuClear) {
            if (!Ready()) return;
            const std::vector<std::array<int, 4>> boxes = {{0, 0, 4, 4}, {4, 0, 4, 4}, {0, 4, 4, 4}};
            const GLuint t = NewTexture();
            const auto p = Pattern(kEdge, 0x40);
            glBindTexture(GL_TEXTURE_2D, t);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kEdge, kEdge, 0, GL_RGBA, GL_UNSIGNED_BYTE, p.data());
            Nearest(GL_TEXTURE_2D, t, 0);
            SyncBySampling(t);
            GpuClear(t, 0, kGreen);
            SyncBySampling(t);
            WriteBlueBoxes(t, boxes);
            ExpectImage(ImageCopy(t, 0, GL_RGBA8, "rgba8"), kEdge, GreenWithBlueBoxes(boxes));
        }

        TEST_F(ImageBindableRemintScenario, GeneratedMipLevelSurvives) {
            if (!Ready()) return;
            const GLuint t = NewTexture();
            const auto s = Solid(kEdge, 200, 100, 50, 255);
            glBindTexture(GL_TEXTURE_2D, t);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kEdge, kEdge, 0, GL_RGBA, GL_UNSIGNED_BYTE, s.data());
            glGenerateMipmap(GL_TEXTURE_2D);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glBindTexture(GL_TEXTURE_2D, 0);
            SyncBySampling(t);
            ExpectImage(ImageCopy(t, 1, GL_RGBA8, "rgba8"), kEdge / 2, Solid(kEdge / 2, 200, 100, 50, 255));
        }

        TEST_F(ImageBindableRemintScenario, GpuClearOfAnUploadedMipLevelSurvives) {
            if (!Ready()) return;
            const GLuint t = NewTexture();
            const auto s0 = Solid(kEdge, 200, 100, 50, 255);
            const auto s1 = Solid(kEdge / 2, 10, 20, 30, 255);
            glBindTexture(GL_TEXTURE_2D, t);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kEdge, kEdge, 0, GL_RGBA, GL_UNSIGNED_BYTE, s0.data());
            glTexImage2D(GL_TEXTURE_2D, 1, GL_RGBA8, kEdge / 2, kEdge / 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, s1.data());
            Nearest(GL_TEXTURE_2D, t, 1);
            SyncBySampling(t);
            GpuClear(t, 1, kGreen);
            SyncBySampling(t);
            ExpectImage(ImageCopy(t, 1, GL_RGBA8, "rgba8"), kEdge / 2, Solid(kEdge / 2, 0, 255, 0, 255));
        }

        // Control: a level defined (null data) after the last sync is not on the driver yet, so
        // there is nothing to read back for it; the bound level 0 must still be the upload.
        TEST_F(ImageBindableRemintScenario, LevelDefinedAfterTheSyncDoesNotDisturbTheOthers) {
            if (!Ready()) return;
            const GLuint t = NewTexture();
            const auto p = Pattern(kEdge, 0x40);
            glBindTexture(GL_TEXTURE_2D, t);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kEdge, kEdge, 0, GL_RGBA, GL_UNSIGNED_BYTE, p.data());
            Nearest(GL_TEXTURE_2D, t, 0);
            SyncBySampling(t);
            glBindTexture(GL_TEXTURE_2D, t);
            glTexImage2D(GL_TEXTURE_2D, 1, GL_RGBA8, kEdge / 2, kEdge / 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glBindTexture(GL_TEXTURE_2D, 0);
            ExpectImage(ImageCopy(t, 0, GL_RGBA8, "rgba8"), kEdge, p);
        }

        // Control: the same late level, partly uploaded - the shadow is the only copy of it.
        TEST_F(ImageBindableRemintScenario, LevelDefinedAndUploadedAfterTheSyncKeepsTheUpload) {
            if (!Ready()) return;
            const GLuint t = NewTexture();
            const auto p = Pattern(kEdge, 0x40);
            glBindTexture(GL_TEXTURE_2D, t);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kEdge, kEdge, 0, GL_RGBA, GL_UNSIGNED_BYTE, p.data());
            Nearest(GL_TEXTURE_2D, t, 0);
            SyncBySampling(t);
            glBindTexture(GL_TEXTURE_2D, t);
            glTexImage2D(GL_TEXTURE_2D, 1, GL_RGBA8, kEdge / 2, kEdge / 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            const auto blue = Solid(2, 0, 0, 255, 255);
            glTexSubImage2D(GL_TEXTURE_2D, 1, 0, 0, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, blue.data());
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 1);
            glBindTexture(GL_TEXTURE_2D, 0);
            ExpectImage(ImageCopy(t, 1, GL_RGBA8, "rgba8"), 2, blue);
        }

        // GL_RG8 is not an ESSL image format, so the re-mint also WIDENS the storage (to RGBA8)
        // and cannot keep the old allocation even though it is immutable.
        TEST_F(ImageBindableRemintScenario, GpuClearSurvivesAWideningRemint) {
            if (!Ready()) return;
            const GLuint t = NewTexture();
            std::vector<GLubyte> rg(static_cast<std::size_t>(kEdge * kEdge) * 2u, 0x33);
            glBindTexture(GL_TEXTURE_2D, t);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_RG8, kEdge, kEdge);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kEdge, kEdge, GL_RG, GL_UNSIGNED_BYTE, rg.data());
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            Nearest(GL_TEXTURE_2D, t, 0);
            SyncBySampling(t);
            const GLfloat c[4] = {0.5f, 0.25f, 0.0f, 1.0f};
            GpuClear(t, 0, c);
            SyncBySampling(t);
            ExpectImage(ImageCopy(t, 0, GL_RG8, "rg8"), kEdge, Solid(kEdge, 128, 64, 0, 255), 2);
        }

        // Control: widened, uploaded only.
        TEST_F(ImageBindableRemintScenario, UploadSurvivesAWideningRemint) {
            if (!Ready()) return;
            const GLuint t = NewTexture();
            std::vector<GLubyte> rg(static_cast<std::size_t>(kEdge * kEdge) * 2u);
            for (std::size_t i = 0; i < rg.size(); i += 2) {
                rg[i] = 0x33;
                rg[i + 1] = 0x99;
            }
            glBindTexture(GL_TEXTURE_2D, t);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_RG8, kEdge, kEdge);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kEdge, kEdge, GL_RG, GL_UNSIGNED_BYTE, rg.data());
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            Nearest(GL_TEXTURE_2D, t, 0);
            SyncBySampling(t);
            ExpectImage(ImageCopy(t, 0, GL_RG8, "rg8"), kEdge, Solid(kEdge, 0x33, 0x99, 0, 255), 2);
        }

        // Control: a signed-normalized level (not colour-renderable on every ES driver, so its
        // readback may be refused and the shadow used instead), uploaded only.
        TEST_F(ImageBindableRemintScenario, SnormUploadSurvives) {
            if (!Ready()) return;
            const GLuint t = NewTexture();
            std::vector<GLbyte> s(static_cast<std::size_t>(kEdge * kEdge) * 4u);
            for (std::size_t i = 0; i < s.size(); i += 4) {
                s[i] = 127;
                s[i + 1] = 0;
                s[i + 2] = -127;
                s[i + 3] = 127;
            }
            glBindTexture(GL_TEXTURE_2D, t);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8_SNORM, kEdge, kEdge, 0, GL_RGBA, GL_BYTE, s.data());
            Nearest(GL_TEXTURE_2D, t, 0);
            SyncBySampling(t);
            ExpectImage(ImageCopy(t, 0, GL_RGBA8_SNORM, "rgba8_snorm", true), kEdge, Solid(kEdge, 255, 128, 0, 255),
                        3);
        }

        // Control: RGBA16_SNORM is desktop-only as an image format, so this re-mint widens too.
        TEST_F(ImageBindableRemintScenario, Snorm16UploadSurvivesAWideningRemint) {
            if (!Ready()) return;
            const GLuint t = NewTexture();
            std::vector<GLshort> s(static_cast<std::size_t>(kEdge * kEdge) * 4u);
            for (std::size_t i = 0; i < s.size(); i += 4) {
                s[i] = 32767;
                s[i + 1] = 0;
                s[i + 2] = -32767;
                s[i + 3] = 32767;
            }
            glBindTexture(GL_TEXTURE_2D, t);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA16_SNORM, kEdge, kEdge);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kEdge, kEdge, GL_RGBA, GL_SHORT, s.data());
            Nearest(GL_TEXTURE_2D, t, 0);
            SyncBySampling(t);
            ExpectImage(ImageCopy(t, 0, GL_RGBA16_SNORM, "rgba16_snorm", true), kEdge,
                        Solid(kEdge, 255, 128, 0, 255), 3);
        }

        // An array texture is read back one layer at a time: the GPU-cleared layer and the
        // uploaded one both have to come through. The clear's FBO attach is what gives the
        // texture its driver storage here.
        TEST_F(ImageBindableRemintScenario, GpuClearOfOneArrayLayerSurvivesBesideTheUploadedLayer) {
            if (!Ready()) return;
            const GLuint t = NewTexture();
            const auto p = Pattern(kEdge, 0x40);
            std::vector<GLubyte> layers(p);
            layers.insert(layers.end(), p.begin(), p.end());
            glBindTexture(GL_TEXTURE_2D_ARRAY, t);
            glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, kEdge, kEdge, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                         layers.data());
            glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
            Nearest(GL_TEXTURE_2D_ARRAY, t, 0);
            GpuClear(t, 0, kGreen, /*layer=*/1);
            glFinish();
            ExpectImage(ImageCopy(t, 0, GL_RGBA8, "rgba8", false, /*layer=*/1), kEdge, Solid(kEdge, 0, 255, 0, 255));
            ExpectImage(ImageCopy(t, 0, GL_RGBA8, "rgba8", false, /*layer=*/0), kEdge, p);
        }

        // A view owns no storage, so its first image binding is the VIEWED texture's transition,
        // and the GPU's texels there have to come through it. Needs glTextureView, which Espryt
        // only has where the ES driver does (not on llvmpipe).
        TEST_F(ImageBindableRemintScenario, GpuClearSurvivesTheFirstImageBindingOfAView) {
            if (!Ready()) return;
            const GLuint owner = NewTexture();
            const auto p = Pattern(kEdge, 0x40);
            glBindTexture(GL_TEXTURE_2D, owner);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, kEdge, kEdge);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kEdge, kEdge, GL_RGBA, GL_UNSIGNED_BYTE, p.data());
            Nearest(GL_TEXTURE_2D, owner, 0);
            SyncBySampling(owner);
            GpuClear(owner, 0, kGreen);
            SyncBySampling(owner);
            const GLuint view = NewTexture();
            FirstGLError();
            glTextureView(view, GL_TEXTURE_2D, owner, GL_RGBA8, 0, 1, 0, 1);
            if (const GLenum error = FirstGLError()) {
                GTEST_SKIP() << "glTextureView is unavailable on backend " << Gl().BackendName() << " ("
                             << GLErrorName(error) << ")";
            }
            Nearest(GL_TEXTURE_2D, view, 0);
            SyncBySampling(view);
            ExpectImage(ImageCopy(view, 0, GL_RGBA8, "rgba8"), kEdge, Solid(kEdge, 0, 255, 0, 255));
        }
    } // namespace
} // namespace MGITest
