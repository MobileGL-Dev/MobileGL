// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/BufferTextureScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - A BUFFER TEXTURE IS SAMPLED FROM THE VERTEX STAGE, AND TRACKS ITS BUFFER.
//
// Buffer textures are core in OpenGL 3.1 and MobileGL advertises a 4.x context, so an
// application may build geometry out of one without asking whether the host can. Minecraft
// 26.3 does exactly that: its cloud layer has no vertex attributes at all, only gl_VertexID
// and texelFetch on a GL_R8I buffer texture. Nothing covered that path end to end on either
// backend - the frontend unit tests stop at glTexBuffer's state, and no scenario ever drew
// with the result - which is how DirectGLES came to emit `#extension GL_EXT_texture_buffer :
// require` unconditionally, compile nothing on a host without the extension, and lose the
// whole cloud layer with no diagnostic anywhere.
//
// Two claims, in the order they can break:
//   1. a vertex-stage texelFetch on an R8I buffer texture reads the byte the application put
//      in the buffer (the shape of the real workload: no attributes, index from gl_VertexID);
//   2. a later glBufferSubData is visible to the next draw WITHOUT re-specifying the texture.
//      glTexBuffer attaches storage, it does not copy: the texture is a live view of the
//      buffer, so a backend that only refreshes the view when the texture's own state changes
//      must still show the new bytes. DirectGLES' respecify gate is keyed on the texture info
//      and deliberately does not include the buffer's contents, so this is the assertion that
//      says that is safe rather than merely untested.
//
// NOTE ON A HOST WITHOUT BUFFER TEXTURES: this scenario is expected to FAIL there, and that is
// the honest outcome - MobileGL keeps advertising GL_MAX_TEXTURE_BUFFER_SIZE (an OpenGL 4.x
// context may not answer 0), so there is no capability an application, or this test, could
// branch on. The driver POST's "Buffer textures" row is where that verdict is stated.

#include <cstdint>
#include <cstring>
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

        // No vertex attributes: the quad's corners come from gl_VertexID, exactly like the
        // workload this exists for. The texel is fetched in the VERTEX stage - the stage where
        // buffer-texture support is scarcest across ES drivers - and carried flat so every
        // fragment of the quad reports the same byte and the readback is exact.
        constexpr const char* kVS = R"(#version 330 core
uniform isamplerBuffer uFaces;
flat out int vFace;
void main() {
    vec2 corner = vec2((gl_VertexID & 1) == 0 ? -1.0 : 1.0,
                       (gl_VertexID & 2) == 0 ? -1.0 : 1.0);
    vFace = texelFetch(uFaces, 0).r;
    gl_Position = vec4(corner, 0.0, 1.0);
}
)";

        // 1/255 steps survive an RGBA8 round trip exactly, so the readback byte IS the value
        // the vertex shader fetched.
        constexpr const char* kFS = R"(#version 330 core
flat in int vFace;
out vec4 o_color;
void main() { o_color = vec4(float(vFace) / 255.0, 0.0, 0.0, 1.0); }
)";

        // A buffer texture bound as a WRITABLE image: the shader reads one texel and writes
        // another, so a single dispatch proves the read direction (which already worked) and
        // the write direction (which is what this exists for) apart from each other.
        constexpr const char* kImageBufferCS = R"(#version 430 core
layout(local_size_x = 1) in;
layout(binding = 0, rgba8) uniform imageBuffer uImage;
void main() {
    vec4 read = imageLoad(uImage, 1);
    imageStore(uImage, 0, vec4(0.0, 1.0, 0.0, 1.0));
    imageStore(uImage, 2, read);
}
)";

        class BufferTextureScenario : public ScenarioTest {
        protected:
            bool ComputeImagesAreUsable() const {
                GLint maxImageUnits = 0;
                GLint maxComputeImageUniforms = 0;
                glGetIntegerv(GL_MAX_IMAGE_UNITS, &maxImageUnits);
                glGetIntegerv(GL_MAX_COMPUTE_IMAGE_UNIFORMS, &maxComputeImageUniforms);
                while (glGetError() != GL_NO_ERROR) {
                }
                return maxImageUnits >= 1 && maxComputeImageUniforms >= 1;
            }

            unsigned int MakeComputeProgram(const char* source) {
                const GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
                glShaderSource(shader, 1, &source, nullptr);
                glCompileShader(shader);
                GLint compiled = GL_FALSE;
                glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
                if (compiled == GL_FALSE) {
                    char log[4096] = {};
                    glGetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
                    ADD_FAILURE() << "the compute shader did not compile: " << log;
                    glDeleteShader(shader);
                    return 0;
                }
                const GLuint program = glCreateProgram();
                glAttachShader(program, shader);
                glLinkProgram(program);
                glDeleteShader(shader);
                GLint linked = GL_FALSE;
                glGetProgramiv(program, GL_LINK_STATUS, &linked);
                if (linked == GL_FALSE) {
                    char log[4096] = {};
                    glGetProgramInfoLog(program, sizeof(log) - 1, nullptr, log);
                    ADD_FAILURE() << "the compute program did not link: " << log;
                    glDeleteProgram(program);
                    return 0;
                }
                return program;
            }
        };

        // Draws the full-viewport quad and returns the red byte every fragment was painted with,
        // or -1 if the quad did not come out uniform (which would mean the flat varying, not the
        // fetch, is what this test is measuring).
        int PaintedValue(unsigned int program, int width, int height) {
            ClearTo(0.0f, 0.0f, 0.0f, 1.0f);
            GLuint vao = 0;
            glGenVertexArrays(1, &vao);
            glBindVertexArray(vao);
            glUseProgram(program);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            glBindVertexArray(0);
            glDeleteVertexArrays(1, &vao);

            const Image image = ReadPixels(width, height);
            if (image.Empty()) {
                return -1;
            }
            const int first = image.At(0, 0).r;
            for (int y = 0; y < image.Height(); ++y) {
                for (int x = 0; x < image.Width(); ++x) {
                    if (image.At(x, y).r != first) {
                        return -1;
                    }
                }
            }
            return first;
        }

    } // namespace

    TEST_F(BufferTextureScenario, VertexStageTexelFetchReadsTheBufferAndTracksItsUpdates) {
        if (!Ready()) return;
        HeadlessGL& gl = Gl();

        std::string error;
        const unsigned int program = CompileProgram(kVS, kFS, &error);
        ASSERT_NE(program, 0u) << error;

        // GL_R8I is the format the real workload uses. Signed, so the values stay well inside
        // [0, 127] to keep the readback arithmetic honest.
        constexpr signed char kInitial = 37;
        constexpr signed char kUpdated = 91;
        std::vector<signed char> texels(64, 0);
        texels[0] = kInitial;

        // The harness shares one context across every scenario in the process, so an error left
        // by an earlier one would surface below as "glTexBuffer was refused".
        FirstGLError();

        GLuint buffer = 0;
        glGenBuffers(1, &buffer);
        glBindBuffer(GL_TEXTURE_BUFFER, buffer);
        glBufferData(GL_TEXTURE_BUFFER, static_cast<GLsizeiptr>(texels.size()), texels.data(),
                     GL_DYNAMIC_DRAW);

        GLuint texture = 0;
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_BUFFER, texture);
        glTexBuffer(GL_TEXTURE_BUFFER, GL_R8I, buffer);
        ASSERT_EQ(FirstGLError(), 0u) << "glTexBuffer(GL_R8I) was refused";

        ColorFbo target = MakeColorFbo(64, 64);
        ASSERT_NE(target.fbo, 0u) << "could not create the render target";
        BindFbo(target);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_BUFFER, texture);
        glUseProgram(program);
        const GLint location = glGetUniformLocation(program, "uFaces");
        ASSERT_NE(location, -1) << "the buffer sampler was optimized away or never reflected";
        glUniform1i(location, 0);

        EXPECT_EQ(PaintedValue(program, target.width, target.height), static_cast<int>(kInitial))
            << "a vertex-stage texelFetch on an R8I buffer texture did not read the byte the "
               "application stored (a uniform -1 here means the quad was not uniform at all)";

        // The texture is a VIEW of the buffer: no glTexBuffer call follows, and none should be
        // needed for the new bytes to be visible.
        glBindBuffer(GL_TEXTURE_BUFFER, buffer);
        glBufferSubData(GL_TEXTURE_BUFFER, 0, 1, &kUpdated);
        ASSERT_EQ(FirstGLError(), 0u) << "glBufferSubData on the texture's buffer was refused";

        EXPECT_EQ(PaintedValue(program, target.width, target.height), static_cast<int>(kUpdated))
            << "the buffer texture kept showing the old contents after glBufferSubData; the "
               "texture must track its buffer without being re-specified";

        BindDefaultFramebuffer();
        DestroyColorFbo(target);
        glUseProgram(0);
        glDeleteProgram(program);
        glDeleteTextures(1, &texture);
        glDeleteBuffers(1, &buffer);
        glViewport(0, 0, gl.Width(), gl.Height());
        EXPECT_EQ(FirstGLError(), 0u);
    }

    // A shader may WRITE a buffer texture too, through an image unit, and the bytes it writes
    // land in the backend's buffer - not in the frontend's CPU shadow, which is what MapBuffer
    // and GetBufferSubData hand back. A storage-block write is flagged for exactly this reason
    // and the shadow is refreshed on the next read; a buffer reached through an image unit is
    // the same write through a different binding, and Espryt used to flag only the first, so
    // an imageStore into a buffer texture was invisible to every CPU read that followed it -
    // silently, with the correct value sitting in the driver's buffer the whole time.
    //
    // The read direction is asserted in the same dispatch (texel 2 is a copy of texel 1) so a
    // failure here cannot be blamed on the image binding not working at all.
    TEST_F(BufferTextureScenario, AnImageStoreIntoABufferTextureIsVisibleToTheCpu) {
        if (!Ready()) return;
        if (!ComputeImagesAreUsable()) GTEST_SKIP() << "no compute image units on this host";

        constexpr GLuint kRed = 0x000000ffu;   // RGBA8 little-endian: r = 255
        constexpr GLuint kGreen = 0xff00ff00u; // what the shader stores: (0, 1, 0, 1)
        constexpr int kTexels = 16;

        FirstGLError();

        const unsigned int program = MakeComputeProgram(kImageBufferCS);
        ASSERT_NE(program, 0u);

        const std::vector<GLuint> texels(kTexels, kRed);
        GLuint buffer = 0;
        glGenBuffers(1, &buffer);
        glBindBuffer(GL_TEXTURE_BUFFER, buffer);
        glBufferData(GL_TEXTURE_BUFFER, static_cast<GLsizeiptr>(texels.size() * sizeof(GLuint)), texels.data(),
                     GL_DYNAMIC_COPY);

        GLuint texture = 0;
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_BUFFER, texture);
        glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA8, buffer);
        EXPECT_EQ(FirstGLError(), 0u) << "glTexBuffer(GL_RGBA8) was refused";

        glBindImageTexture(0, texture, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA8);
        EXPECT_EQ(FirstGLError(), 0u) << "glBindImageTexture on a buffer texture was refused";

        glUseProgram(program);
        glDispatchCompute(1, 1, 1);
        glMemoryBarrier(GL_ALL_BARRIER_BITS);

        // Both CPU read paths, because they are two entry points onto the same refresh and a
        // fix that reaches only one of them is not a fix. Everything below is EXPECT rather than
        // ASSERT so that a failure still reaches the cleanup at the end: the harness shares one
        // context across every scenario in the process, and a leaked buffer or image binding
        // here would surface as a failure somewhere else entirely.
        std::vector<GLuint> readBack(kTexels, 0u);
        glBindBuffer(GL_TEXTURE_BUFFER, buffer);
        glGetBufferSubData(GL_TEXTURE_BUFFER, 0, static_cast<GLsizeiptr>(readBack.size() * sizeof(GLuint)),
                           readBack.data());
        EXPECT_EQ(readBack[0], kGreen) << "glGetBufferSubData did not see the imageStore";
        EXPECT_EQ(readBack[2], kRed) << "the imageLoad side of the same dispatch read the wrong texel";

        const void* mapped = glMapBuffer(GL_TEXTURE_BUFFER, GL_READ_ONLY);
        EXPECT_NE(mapped, nullptr) << "glMapBuffer(GL_READ_ONLY) on the texture's buffer failed";
        if (mapped != nullptr) {
            GLuint mappedTexel0 = 0;
            std::memcpy(&mappedTexel0, mapped, sizeof(mappedTexel0));
            EXPECT_EQ(mappedTexel0, kGreen) << "glMapBuffer did not see the imageStore";
            glUnmapBuffer(GL_TEXTURE_BUFFER);
        }

        glBindImageTexture(0, 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);
        glBindBuffer(GL_TEXTURE_BUFFER, 0);
        glBindTexture(GL_TEXTURE_BUFFER, 0);
        glUseProgram(0);
        glDeleteProgram(program);
        glDeleteTextures(1, &texture);
        glDeleteBuffers(1, &buffer);
        EXPECT_EQ(FirstGLError(), 0u);
    }

} // namespace MGITest
