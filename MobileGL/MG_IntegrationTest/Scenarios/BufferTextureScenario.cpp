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

        class BufferTextureScenario : public ScenarioTest {};

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

} // namespace MGITest
