// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/CubeFaceReadbackScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// P13 W6: ONE CUBE FACE OUT, BY TOKEN AND BY Z OFFSET. Two GL 4.6 readback rules that used to be
// pinned by unit cases on the monolith frontend arm (TextureTest / TextureViewTest), which read the
// frontend's level shadow. That arm is gone; every library now answers a readback from the
// backend's own image (the record arm), and a unit process has no backend image to read. So the
// rules are asserted here, on a real context, on every arm the scenarios run on:
//
//   * glGetTexImage of ONE cube face packs one face (GL 4.6 8.11.4), so a GL_PIXEL_PACK_BUFFER
//     sized for one face is exactly big enough - the validator once measured it against all six
//     faces and refused with INVALID_OPERATION - and the face it holds is the one the token named.
//   * glGetTextureSubImage of a cube-map VIEW over a 2D array names the face by its z offset
//     (GL 4.6 8.11.4, Table 8.25), and face f of a view based at layer L is array layer L + f.
//
// Every face and every layer carries its own value, so a read that lands on the wrong one says
// which one answered instead of merely failing.

#include <cstdint>
#include <cstring>
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

        class CubeFaceReadbackScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                DrainErrors();
                glPixelStorei(GL_PACK_ALIGNMENT, 4);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            }

            void TearDown() override {
                if (!Ready()) return;
                glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
                for (GLuint buffer : m_buffers) glDeleteBuffers(1, &buffer);
                for (GLuint texture : m_textures) glDeleteTextures(1, &texture);
                m_buffers.clear();
                m_textures.clear();
                DrainErrors();
                ScenarioTest::TearDown();
            }

            GLuint NewTexture() {
                GLuint texture = 0;
                glGenTextures(1, &texture);
                m_textures.push_back(texture);
                return texture;
            }

            GLuint NewBuffer() {
                GLuint buffer = 0;
                glGenBuffers(1, &buffer);
                m_buffers.push_back(buffer);
                return buffer;
            }

            static bool HasExtension(const char* name) {
                GLint count = 0;
                glGetIntegerv(GL_NUM_EXTENSIONS, &count);
                for (GLint i = 0; i < count; ++i) {
                    const auto* ext = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i)));
                    if (ext != nullptr && std::strcmp(ext, name) == 0) return true;
                }
                return false;
            }

            static void DrainErrors() {
                for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; ++i) {
                }
            }

            std::vector<GLuint> m_buffers;
            std::vector<GLuint> m_textures;
        };

        TEST_F(CubeFaceReadbackScenario, GetTexImageOfOneCubeFacePacksIntoAOneFacePixelPackBuffer) {
            if (!Ready()) return;
            constexpr GLsizei kEdge = 2;
            constexpr GLsizeiptr kFaceBytes = kEdge * kEdge * 4;

            const GLuint texture = NewTexture();
            glBindTexture(GL_TEXTURE_CUBE_MAP, texture);
            glTexStorage2D(GL_TEXTURE_CUBE_MAP, 1, GL_RGBA8, kEdge, kEdge);
            for (int face = 0; face < 6; ++face) {
                std::vector<GLubyte> seed(static_cast<size_t>(kFaceBytes), static_cast<GLubyte>(10 + face));
                glTexSubImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, 0, 0, 0, kEdge, kEdge, GL_RGBA,
                                GL_UNSIGNED_BYTE, seed.data());
            }
            ASSERT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR)) << "seeding the six faces failed";

            const GLuint buffer = NewBuffer();
            glBindBuffer(GL_PIXEL_PACK_BUFFER, buffer);
            glBufferData(GL_PIXEL_PACK_BUFFER, kFaceBytes, nullptr, GL_STREAM_READ);
            ASSERT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR)) << "creating the one-face pack buffer failed";

            glGetTexImage(GL_TEXTURE_CUBE_MAP_NEGATIVE_Z, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR))
                << "a pixel pack buffer sized for the one face this call packs was refused";

            std::vector<GLubyte> packed(static_cast<size_t>(kFaceBytes), 0);
            glGetBufferSubData(GL_PIXEL_PACK_BUFFER, 0, kFaceBytes, packed.data());
            for (size_t i = 0; i < packed.size(); ++i) {
                ASSERT_EQ(static_cast<int>(packed[i]), 15)
                    << "byte " << i << ": the pack buffer holds face " << (static_cast<int>(packed[i]) - 10)
                    << ", not -Z (face 5)";
            }
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
        }

        TEST_F(CubeFaceReadbackScenario, CubeMapViewOfAnArrayReadsTheFaceEachZOffsetNames) {
            if (!Ready()) return;
            constexpr GLint kLayers = 8;
            constexpr GLint kViewMinLayer = 2;

            // Espryt mints views through the ES driver's texture-view extension and, without it,
            // refuses glTextureView with INVALID_OPERATION and withholds GL_ARB_texture_view rather
            // than emulate by copying (TextureViewScenario's header) - no view to read through.
            if (!HasExtension("GL_ARB_texture_view")) {
                GTEST_SKIP() << "GL_ARB_texture_view is not advertised on backend " << Gl().BackendName();
            }
            const GLuint storage = NewTexture();
            glBindTexture(GL_TEXTURE_2D_ARRAY, storage);
            glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_RGBA8, 1, 1, kLayers);
            for (GLint layer = 0; layer < kLayers; ++layer) {
                const GLubyte texel[] = {static_cast<GLubyte>(10 + layer), 20, 30, 40};
                glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, layer, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, texel);
            }
            ASSERT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR)) << "seeding the array failed";

            const GLuint view = NewTexture();
            glTextureView(view, GL_TEXTURE_CUBE_MAP, storage, GL_RGBA8, 0, 1, kViewMinLayer, 6);
            ASSERT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR)) << "the cube-map view over the array was refused";

            for (GLint face = 0; face < 6; ++face) {
                GLubyte output[4] = {};
                glGetTextureSubImage(view, 0, 0, 0, face, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, sizeof(output), output);
                EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR)) << "reading face " << face << " errored";
                EXPECT_EQ(static_cast<GLint>(output[0]), 10 + kViewMinLayer + face)
                    << "face " << face << " of a view based at layer " << kViewMinLayer << " answered with layer "
                    << (static_cast<GLint>(output[0]) - 10);
            }
        }

    } // namespace
} // namespace MGITest
