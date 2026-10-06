// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/CopyImageStoreReadbackScenario.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - glCopyImageSubData INTO A LEVEL THE DRIVER WILL NOT READ BACK (P8-E,
// docs/Disaggregated/notes/p8/E.md).
//
// The formats are the ones an ES driver keeps as texture-only storage: GL_RGB16 (EXT_texture_norm16
// makes the three-channel spelling sampleable, never colour-renderable) and the 16-bit SNORM family
// (renderable only with EXT_render_snorm, which Adreno does not have). Their glGetTexImage cannot go
// through a framebuffer on such a driver, so something other than the GPU image has to answer it:
// the CPU shadow on the monolith arm, the server's staged store on a split one. A copy writes the
// GPU image and nothing else, which is exactly the write such a byte store never sees.
//
// GL_RGB9_E5 is the positive control: a 32-bit packed format, read through the scratch GL_R32UI
// image whoever wrote the level, so it must stay green under every configuration below.
//
// ARMS. Both backends on the monolith arm and on Split / Spawn / Tcp, every case, with no knob:
// on llvmpipe every one of these formats is attachable, so these runs pin the GPU route and the
// copy itself. Magma is the control backend - it reads the VkImage back whatever the format, so it
// has no store route to exercise.
//
// THE KNOB. MGITEST_ESPRYT_REFUSE_TEXTURE_READBACK_EXTENT=<w>x<h> is read BY THE ESPRYT SERVER
// (WireTextureReadback.inc, ReadTextureImageWire): a colour level of exactly that extent is
// treated as a level the driver refused to attach, which is what an Adreno does for every one of
// these formats and what llvmpipe can be made to do no other way. The `DirectGLES.<arm>.StoreRead.`
// entries set it, and this scenario then builds its textures at THAT extent instead of the default
// one - so the knob is scoped by the extent, and the tcp lane's shared supervisor can carry it
// (the TcpServer.Start fixture's ENVIRONMENT) without touching any other entry, this scenario's
// own knob-free tcp entries included. Under the knob every read must still be right, and the
// server log must name the store route - otherwise the entry would be pinning the GPU route a
// second time under a name that claims otherwise.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

        constexpr const char* kRefuseExtentKnob = "MGITEST_ESPRYT_REFUSE_TEXTURE_READBACK_EXTENT";
        // What the Espryt server logs (once per process) when it answers a read from its store.
        constexpr const char* kStoreReadMarker = "texture-readback-from-store";
        // The knob-free extent. Deliberately not the extent the knob entries use, so the tcp
        // supervisor - which carries the knob for the whole lane - leaves these textures alone.
        constexpr int kDefaultWidth = 12;
        constexpr int kDefaultHeight = 6;

        struct CopyFormat {
            GLenum internalFormat;
            GLenum format;
            GLenum type;
            int components; // 16-bit components per texel; 0 for the one 32-bit packed format
            bool isSigned;
            const char* name;
        };

        const CopyFormat kFormats[] = {
            {GL_RGB16, GL_RGB, GL_UNSIGNED_SHORT, 3, false, "rgb16"},
            {GL_RGBA16_SNORM, GL_RGBA, GL_SHORT, 4, true, "rgba16_snorm"},
            {GL_RGB16_SNORM, GL_RGB, GL_SHORT, 3, true, "rgb16_snorm"},
            {GL_RGBA16, GL_RGBA, GL_UNSIGNED_SHORT, 4, false, "rgba16"},
            {GL_RGB9_E5, GL_RGB, GL_UNSIGNED_INT_5_9_9_9_REV, 0, false, "rgb9_e5"},
        };

        int TexelBytes(const CopyFormat& format) { return format.components == 0 ? 4 : format.components * 2; }

        // Every word differs from its neighbours and from the other patterns, so a mis-addressed
        // row, a swapped endpoint or a copy that never happened cannot cancel out. SNORM never
        // gets -32768: it and -32767 are the same value (-1.0), and a read that goes through a
        // normalized float is entitled to answer either.
        std::vector<std::uint8_t> MakeTexels(const CopyFormat& format, int width, int height, std::uint32_t seed) {
            const std::size_t texels = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
            std::vector<std::uint8_t> bytes(texels * static_cast<std::size_t>(TexelBytes(format)));
            if (format.components == 0) {
                for (std::size_t i = 0; i < texels; ++i) {
                    std::uint32_t word = (static_cast<std::uint32_t>(i) + 1u) * 2654435761u ^ seed * 40503u;
                    word ^= word >> 13;
                    std::memcpy(bytes.data() + i * 4, &word, 4);
                }
                return bytes;
            }
            const std::size_t words = texels * static_cast<std::size_t>(format.components);
            for (std::size_t i = 0; i < words; ++i) {
                std::uint32_t mix = (static_cast<std::uint32_t>(i) + 1u) * 2654435761u ^ seed * 40503u;
                mix ^= mix >> 15;
                std::uint16_t value = static_cast<std::uint16_t>(mix >> 8);
                if (format.isSigned && value == 0x8000u) value = 0x8001u;
                std::memcpy(bytes.data() + i * 2, &value, 2);
            }
            return bytes;
        }

        // `image` with the w x h block at (dx, dy) replaced by the block of `source` at (sx, sy).
        std::vector<std::uint8_t> WithBlock(std::vector<std::uint8_t> image, int width, const std::vector<std::uint8_t>& source,
                                            int sourceWidth, int sx, int sy, int dx, int dy, int w, int h,
                                            int texelBytes) {
            for (int row = 0; row < h; ++row) {
                std::memcpy(image.data() + (static_cast<std::size_t>(dy + row) * width + dx) * texelBytes,
                            source.data() + (static_cast<std::size_t>(sy + row) * sourceWidth + sx) * texelBytes,
                            static_cast<std::size_t>(w) * texelBytes);
            }
            return image;
        }

        class CopyImageStoreReadbackScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                m_wire = SplitRuntimeSkipReason().empty();
                // P13 W4b: monolith on the record arm reads back through the server's store route.
                m_recordArm = m_wire || PeekSplitRuntime().dataArmIsRecord;
                m_espryt = Gl().BackendName() == "DirectGLES";
                if (const char* knob = std::getenv(kRefuseExtentKnob)) {
                    int width = 0, height = 0;
                    ASSERT_EQ(std::sscanf(knob, "%dx%d", &width, &height), 2)
                        << kRefuseExtentKnob << "=" << knob << " is not <w>x<h>";
                    ASSERT_GE(width, 12) << "the copy windows below need at least a 12x6 level";
                    ASSERT_GE(height, 6) << "the copy windows below need at least a 12x6 level";
                    m_width = width;
                    m_height = height;
                    m_forced = true;
                }
                // 16-bit RGB rows are 6 bytes a texel: the default 4-byte alignment would pad every
                // row of an odd-width level and shear both the upload and the read.
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                ASSERT_EQ(FirstGLError(), 0u);
            }

            void TearDown() override {
                if (!Ready()) return;
                glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
                glPixelStorei(GL_PACK_ALIGNMENT, 4);
                glBindTexture(GL_TEXTURE_2D, 0);
                if (!m_textures.empty()) {
                    glDeleteTextures(static_cast<GLsizei>(m_textures.size()), m_textures.data());
                }
                if (m_renderbuffer != 0) glDeleteRenderbuffers(1, &m_renderbuffer);
                if (m_fbo != 0) {
                    BindDefaultFramebuffer();
                    glDeleteFramebuffers(1, &m_fbo);
                }
                FirstGLError();
            }

            GLuint MakeTexture(const CopyFormat& format, const std::vector<std::uint8_t>& texels, bool immutable) {
                GLuint texture = 0;
                glGenTextures(1, &texture);
                m_textures.push_back(texture);
                glBindTexture(GL_TEXTURE_2D, texture);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                if (immutable) {
                    glTexStorage2D(GL_TEXTURE_2D, 1, format.internalFormat, m_width, m_height);
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_width, m_height, format.format, format.type,
                                    texels.data());
                } else {
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
                    glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(format.internalFormat), m_width, m_height, 0,
                                 format.format, format.type, texels.data());
                }
                glBindTexture(GL_TEXTURE_2D, 0);
                return texture;
            }

            std::vector<std::uint8_t> ReadTexImage(GLuint texture, const CopyFormat& format) {
                std::vector<std::uint8_t> bytes(
                    static_cast<std::size_t>(m_width) * m_height * static_cast<std::size_t>(TexelBytes(format)), 0xA5);
                glBindTexture(GL_TEXTURE_2D, texture);
                glGetTexImage(GL_TEXTURE_2D, 0, format.format, format.type, bytes.data());
                glBindTexture(GL_TEXTURE_2D, 0);
                return bytes;
            }

            // The first texel that differs names the defect (which half of the copy, which word);
            // the rest would only bury it.
            void ExpectImage(const std::vector<std::uint8_t>& got, const std::vector<std::uint8_t>& want,
                             const CopyFormat& format, const std::string& what) {
                ASSERT_EQ(got.size(), want.size());
                const int texelBytes = TexelBytes(format);
                for (std::size_t at = 0; at < got.size(); ++at) {
                    if (got[at] == want[at]) continue;
                    const std::size_t texel = at / static_cast<std::size_t>(texelBytes);
                    const int x = static_cast<int>(texel % static_cast<std::size_t>(m_width));
                    const int y = static_cast<int>(texel / static_cast<std::size_t>(m_width));
                    std::string gotHex, wantHex;
                    char buffer[4];
                    for (int b = 0; b < texelBytes; ++b) {
                        std::snprintf(buffer, sizeof(buffer), "%02x", got[texel * texelBytes + b]);
                        gotHex += buffer;
                        std::snprintf(buffer, sizeof(buffer), "%02x", want[texel * texelBytes + b]);
                        wantHex += buffer;
                    }
                    ADD_FAILURE() << what << " (" << format.name << ", " << Gl().BackendName() << ", "
                                  << (m_wire ? "split" : "monolith") << (m_forced ? ", store read forced" : "")
                                  << "): texel (" << x << ", " << y << ") reads " << gotHex << ", want " << wantHex;
                    return;
                }
            }

            // Under the knob the Espryt server must have answered from its store at least once;
            // otherwise the entry proves nothing about that route. On monolith's record arm (P13
            // W4b) that server code runs in this process and logs into the whole lane log.
            void ExpectStoreReadNamedSince(const PipeStatsWindow::LogMark& mark) {
                if (!m_forced || !m_espryt || !m_recordArm) return;
                const std::string log = m_wire ? PipeStatsWindow::ReadServerLogSince(mark)
                                               : PipeStatsWindow::ReadLaneLogSince(mark);
                EXPECT_NE(log.find(kStoreReadMarker), std::string::npos)
                    << kRefuseExtentKnob << " is set but the server log carries no '" << kStoreReadMarker
                    << "' line: the knob did not reach the server, or no read took the store route";
            }

            // The copy window: 5x3 texels from (1, 1) of the source to (6, 2) of the destination,
            // inside a 12x6 level and away from the partial upload's (0, 0) 3x2 corner.
            static constexpr int kSrcX = 1, kSrcY = 1, kDstX = 6, kDstY = 2, kCopyW = 5, kCopyH = 3;

            void CopyWindow(GLuint source, GLuint destination) {
                glCopyImageSubData(source, GL_TEXTURE_2D, 0, kSrcX, kSrcY, 0, destination, GL_TEXTURE_2D, 0, kDstX,
                                   kDstY, 0, kCopyW, kCopyH, 1);
            }

            bool m_wire = false;
            bool m_recordArm = false;
            bool m_espryt = false;
            bool m_forced = false;
            int m_width = kDefaultWidth;
            int m_height = kDefaultHeight;
            std::vector<GLuint> m_textures;
            GLuint m_renderbuffer = 0;
            GLuint m_fbo = 0;
        };

        // The control: an uploaded level reads back as uploaded.
        TEST_F(CopyImageStoreReadbackScenario, AnUploadedLevelReadsBackExactly) {
            if (!Ready()) return;
            const auto mark = PipeStatsWindow::MarkLaneLog();
            for (const CopyFormat& format : kFormats) {
                const auto texels = MakeTexels(format, m_width, m_height, 11);
                const GLuint texture = MakeTexture(format, texels, /*immutable=*/false);
                ASSERT_EQ(FirstGLError(), 0u) << format.name << ": setup raised a GL error";
                ExpectImage(ReadTexImage(texture, format), texels, format, "the uploaded level");
                ASSERT_EQ(FirstGLError(), 0u) << format.name << ": glGetTexImage raised a GL error";
            }
            ExpectStoreReadNamedSince(mark);
        }

        // THE CASE THE PACKAGE IS ABOUT: the copy lands in the destination's GPU image only, and the
        // read has to see it - whatever answers the read.
        TEST_F(CopyImageStoreReadbackScenario, ACopiedWindowReadsBackOverTheUpload) {
            if (!Ready()) return;
            const auto mark = PipeStatsWindow::MarkLaneLog();
            for (const CopyFormat& format : kFormats) {
                const auto sourceTexels = MakeTexels(format, m_width, m_height, 23);
                const auto destinationTexels = MakeTexels(format, m_width, m_height, 37);
                const GLuint source = MakeTexture(format, sourceTexels, /*immutable=*/false);
                const GLuint destination = MakeTexture(format, destinationTexels, /*immutable=*/false);
                ASSERT_EQ(FirstGLError(), 0u) << format.name << ": setup raised a GL error";
                CopyWindow(source, destination);
                ASSERT_EQ(FirstGLError(), 0u) << format.name << ": glCopyImageSubData raised a GL error";
                const auto want = WithBlock(destinationTexels, m_width, sourceTexels, m_width, kSrcX, kSrcY, kDstX,
                                            kDstY, kCopyW, kCopyH, TexelBytes(format));
                ExpectImage(ReadTexImage(destination, format), want, format, "the copy destination");
                ExpectImage(ReadTexImage(source, format), sourceTexels, format, "the copy source");
                ASSERT_EQ(FirstGLError(), 0u) << format.name << ": glGetTexImage raised a GL error";
            }
            ExpectStoreReadNamedSince(mark);
        }

        // The same into immutable storage (glTexStorage2D, then the whole level uploaded).
        TEST_F(CopyImageStoreReadbackScenario, ACopiedWindowReadsBackFromAnImmutableLevel) {
            if (!Ready()) return;
            const auto mark = PipeStatsWindow::MarkLaneLog();
            for (const CopyFormat& format : kFormats) {
                const auto sourceTexels = MakeTexels(format, m_width, m_height, 41);
                const auto destinationTexels = MakeTexels(format, m_width, m_height, 43);
                const GLuint source = MakeTexture(format, sourceTexels, /*immutable=*/true);
                const GLuint destination = MakeTexture(format, destinationTexels, /*immutable=*/true);
                ASSERT_EQ(FirstGLError(), 0u) << format.name << ": setup raised a GL error";
                CopyWindow(source, destination);
                ASSERT_EQ(FirstGLError(), 0u) << format.name << ": glCopyImageSubData raised a GL error";
                const auto want = WithBlock(destinationTexels, m_width, sourceTexels, m_width, kSrcX, kSrcY, kDstX,
                                            kDstY, kCopyW, kCopyH, TexelBytes(format));
                ExpectImage(ReadTexImage(destination, format), want, format, "the immutable copy destination");
                ASSERT_EQ(FirstGLError(), 0u) << format.name << ": glGetTexImage raised a GL error";
            }
            ExpectStoreReadNamedSince(mark);
        }

        // A copy, then an ordinary partial upload into the same level elsewhere. The upload is the
        // client's; the copied window is not, and a byte store that took the upload as "the level"
        // would lose it again.
        TEST_F(CopyImageStoreReadbackScenario, APartialUploadAfterTheCopyKeepsTheCopiedWindow) {
            if (!Ready()) return;
            const auto mark = PipeStatsWindow::MarkLaneLog();
            for (const CopyFormat& format : kFormats) {
                const auto sourceTexels = MakeTexels(format, m_width, m_height, 53);
                const auto destinationTexels = MakeTexels(format, m_width, m_height, 59);
                const auto patch = MakeTexels(format, 3, 2, 61);
                const GLuint source = MakeTexture(format, sourceTexels, /*immutable=*/false);
                const GLuint destination = MakeTexture(format, destinationTexels, /*immutable=*/false);
                ASSERT_EQ(FirstGLError(), 0u) << format.name << ": setup raised a GL error";
                CopyWindow(source, destination);
                glBindTexture(GL_TEXTURE_2D, destination);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 3, 2, format.format, format.type, patch.data());
                glBindTexture(GL_TEXTURE_2D, 0);
                ASSERT_EQ(FirstGLError(), 0u) << format.name << ": the copy or the upload raised a GL error";
                auto want = WithBlock(destinationTexels, m_width, sourceTexels, m_width, kSrcX, kSrcY, kDstX, kDstY,
                                      kCopyW, kCopyH, TexelBytes(format));
                want = WithBlock(want, m_width, patch, 3, 0, 0, 0, 0, 3, 2, TexelBytes(format));
                ExpectImage(ReadTexImage(destination, format), want, format, "the copy destination after the upload");
                ASSERT_EQ(FirstGLError(), 0u) << format.name << ": glGetTexImage raised a GL error";
            }
            ExpectStoreReadNamedSince(mark);
        }

        // A RENDERBUFFER endpoint. The split arm resolves it by handle (DirectGLES.cpp,
        // MakeGLESCopyImageEndpoint), which is what retired the "CopyImageSubData+RENDERBUFFER"
        // latch; this is the case that says so on every arm. It has no store to answer from - a
        // renderbuffer's texels were never uploaded - so the knob entries do not run it.
        TEST_F(CopyImageStoreReadbackScenario, ARenderbufferSourceLandsInTheTexture) {
            if (!Ready()) return;
            const CopyFormat format{GL_RGBA16, GL_RGBA, GL_UNSIGNED_SHORT, 4, false, "rgba16"};
            const auto destinationTexels = MakeTexels(format, m_width, m_height, 71);
            const GLuint destination = MakeTexture(format, destinationTexels, /*immutable=*/false);
            glGenRenderbuffers(1, &m_renderbuffer);
            glBindRenderbuffer(GL_RENDERBUFFER, m_renderbuffer);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA16, m_width, m_height);
            glBindRenderbuffer(GL_RENDERBUFFER, 0);
            glGenFramebuffers(1, &m_fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, m_renderbuffer);
            ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE))
                << "the RGBA16 renderbuffer is not attachable";
            // k / 65535 for four 16-bit words: exact through a float clear and a UNORM16 store.
            const std::uint16_t words[4] = {0x1234, 0x5678, 0x9abc, 0xffff};
            const GLfloat color[4] = {words[0] / 65535.0f, words[1] / 65535.0f, words[2] / 65535.0f,
                                      words[3] / 65535.0f};
            glDisable(GL_SCISSOR_TEST);
            glClearBufferfv(GL_COLOR, 0, color);
            BindDefaultFramebuffer();
            ASSERT_EQ(FirstGLError(), 0u) << "setup raised a GL error";
            glCopyImageSubData(m_renderbuffer, GL_RENDERBUFFER, 0, kSrcX, kSrcY, 0, destination, GL_TEXTURE_2D, 0,
                               kDstX, kDstY, 0, kCopyW, kCopyH, 1);
            ASSERT_EQ(FirstGLError(), 0u) << "glCopyImageSubData from the renderbuffer raised a GL error";
            std::vector<std::uint8_t> cleared(static_cast<std::size_t>(m_width) * m_height * 8);
            for (std::size_t texel = 0; texel < cleared.size() / 8; ++texel) {
                std::memcpy(cleared.data() + texel * 8, words, 8);
            }
            const auto want = WithBlock(destinationTexels, m_width, cleared, m_width, kSrcX, kSrcY, kDstX, kDstY,
                                        kCopyW, kCopyH, 8);
            ExpectImage(ReadTexImage(destination, format), want, format, "the texture a renderbuffer was copied into");
            ASSERT_EQ(FirstGLError(), 0u) << "glGetTexImage raised a GL error";
        }

    } // namespace
} // namespace MGITest
