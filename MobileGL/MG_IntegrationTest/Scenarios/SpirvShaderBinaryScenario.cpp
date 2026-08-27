// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/SpirvShaderBinaryScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// Scenario - AN APPLICATION-SUPPLIED SPIR-V MODULE RENDERS, END TO END.
//
// GL_ARB_gl_spirv is core in 4.6 and MobileGL advertises a 4.6 context, but glShaderBinary and
// glSpecializeShader were DECLARE_GL_FUNCTION_STUB entry points: they took their arguments,
// recorded no error and did nothing, and glGetShaderiv(GL_SPIR_V_BINARY) raised GL_INVALID_ENUM.
// Every gl_spirv conformance body died on the first of those two calls.
//
// This scenario is the end-to-end proof that the path now WORKS rather than merely answers: two
// modules that glslang compiled ahead of time (embedded below as words, so the test depends on
// no toolchain at run time), handed to glShaderBinary, specialized with a scale and a channel
// index, linked, drawn, and read back. It runs on both backends and, in CI, on llvmpipe/lavapipe.
//
// The two specialization constants are the load-bearing part. The vertex module scales its
// position by constant id 3 and the fragment module writes 1.0 into the channel named by constant
// id 7 - so a specialization that silently did nothing would leave the default scale of 1.0 (a
// full-viewport quad instead of a quarter-sized one) and the default channel 0 (red instead of
// green), and BOTH would show up in the readback. A "specialization" that merely stored the
// values without folding them in is exactly the failure mode this shape is built to catch.
//
// The GLSL the modules came from:
//     vertex:   layout(location = 0) in vec2 aPos;
//               layout(constant_id = 3) const float uScale = 1.0;
//               void main() { gl_Position = vec4(aPos * uScale, 0.0, 1.0); }
//     fragment: layout(location = 0) out vec4 oColor;
//               layout(constant_id = 7) const int uChannel = 0;
//               void main() { vec4 c = vec4(0,0,0,1); c[uChannel] = 1.0; oColor = c; }
// compiled with `glslangValidator -G --target-env opengl`.

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

#ifndef GL_SHADER_BINARY_FORMAT_SPIR_V
#define GL_SHADER_BINARY_FORMAT_SPIR_V 0x9551
#endif
#ifndef GL_SPIR_V_BINARY
#define GL_SPIR_V_BINARY 0x9552
#endif

namespace MGITest {
    namespace {

        class SpirvShaderBinaryScenario : public ScenarioTest {};

        // 255 words
        const unsigned int kVertexModule[] = {
            0x07230203u, 0x00010000u, 0x0008000bu, 0x00000020u, 0x00000000u, 0x00020011u, 0x00000001u, 0x0006000bu,
            0x00000001u, 0x4c534c47u, 0x6474732eu, 0x3035342eu, 0x00000000u, 0x0003000eu, 0x00000000u, 0x00000001u,
            0x0009000fu, 0x00000000u, 0x00000004u, 0x6e69616du, 0x00000000u, 0x0000000du, 0x00000012u, 0x0000001eu,
            0x0000001fu, 0x00030003u, 0x00000002u, 0x000001c2u, 0x00040005u, 0x00000004u, 0x6e69616du, 0x00000000u,
            0x00060005u, 0x0000000bu, 0x505f6c67u, 0x65567265u, 0x78657472u, 0x00000000u, 0x00060006u, 0x0000000bu,
            0x00000000u, 0x505f6c67u, 0x7469736fu, 0x006e6f69u, 0x00070006u, 0x0000000bu, 0x00000001u, 0x505f6c67u,
            0x746e696fu, 0x657a6953u, 0x00000000u, 0x00070006u, 0x0000000bu, 0x00000002u, 0x435f6c67u, 0x4470696cu,
            0x61747369u, 0x0065636eu, 0x00070006u, 0x0000000bu, 0x00000003u, 0x435f6c67u, 0x446c6c75u, 0x61747369u,
            0x0065636eu, 0x00030005u, 0x0000000du, 0x00000000u, 0x00040005u, 0x00000012u, 0x736f5061u, 0x00000000u,
            0x00040005u, 0x00000014u, 0x61635375u, 0x0000656cu, 0x00050005u, 0x0000001eu, 0x565f6c67u, 0x65747265u,
            0x00444978u, 0x00060005u, 0x0000001fu, 0x495f6c67u, 0x6174736eu, 0x4965636eu, 0x00000044u, 0x00030047u,
            0x0000000bu, 0x00000002u, 0x00050048u, 0x0000000bu, 0x00000000u, 0x0000000bu, 0x00000000u, 0x00050048u,
            0x0000000bu, 0x00000001u, 0x0000000bu, 0x00000001u, 0x00050048u, 0x0000000bu, 0x00000002u, 0x0000000bu,
            0x00000003u, 0x00050048u, 0x0000000bu, 0x00000003u, 0x0000000bu, 0x00000004u, 0x00040047u, 0x00000012u,
            0x0000001eu, 0x00000000u, 0x00040047u, 0x00000014u, 0x00000001u, 0x00000003u, 0x00040047u, 0x0000001eu,
            0x0000000bu, 0x00000005u, 0x00040047u, 0x0000001fu, 0x0000000bu, 0x00000006u, 0x00020013u, 0x00000002u,
            0x00030021u, 0x00000003u, 0x00000002u, 0x00030016u, 0x00000006u, 0x00000020u, 0x00040017u, 0x00000007u,
            0x00000006u, 0x00000004u, 0x00040015u, 0x00000008u, 0x00000020u, 0x00000000u, 0x0004002bu, 0x00000008u,
            0x00000009u, 0x00000001u, 0x0004001cu, 0x0000000au, 0x00000006u, 0x00000009u, 0x0006001eu, 0x0000000bu,
            0x00000007u, 0x00000006u, 0x0000000au, 0x0000000au, 0x00040020u, 0x0000000cu, 0x00000003u, 0x0000000bu,
            0x0004003bu, 0x0000000cu, 0x0000000du, 0x00000003u, 0x00040015u, 0x0000000eu, 0x00000020u, 0x00000001u,
            0x0004002bu, 0x0000000eu, 0x0000000fu, 0x00000000u, 0x00040017u, 0x00000010u, 0x00000006u, 0x00000002u,
            0x00040020u, 0x00000011u, 0x00000001u, 0x00000010u, 0x0004003bu, 0x00000011u, 0x00000012u, 0x00000001u,
            0x00040032u, 0x00000006u, 0x00000014u, 0x3f800000u, 0x0004002bu, 0x00000006u, 0x00000016u, 0x00000000u,
            0x0004002bu, 0x00000006u, 0x00000017u, 0x3f800000u, 0x00040020u, 0x0000001bu, 0x00000003u, 0x00000007u,
            0x00040020u, 0x0000001du, 0x00000001u, 0x0000000eu, 0x0004003bu, 0x0000001du, 0x0000001eu, 0x00000001u,
            0x0004003bu, 0x0000001du, 0x0000001fu, 0x00000001u, 0x00050036u, 0x00000002u, 0x00000004u, 0x00000000u,
            0x00000003u, 0x000200f8u, 0x00000005u, 0x0004003du, 0x00000010u, 0x00000013u, 0x00000012u, 0x0005008eu,
            0x00000010u, 0x00000015u, 0x00000013u, 0x00000014u, 0x00050051u, 0x00000006u, 0x00000018u, 0x00000015u,
            0x00000000u, 0x00050051u, 0x00000006u, 0x00000019u, 0x00000015u, 0x00000001u, 0x00070050u, 0x00000007u,
            0x0000001au, 0x00000018u, 0x00000019u, 0x00000016u, 0x00000017u, 0x00050041u, 0x0000001bu, 0x0000001cu,
            0x0000000du, 0x0000000fu, 0x0003003eu, 0x0000001cu, 0x0000001au, 0x000100fdu, 0x00010038u,
        };

        // 134 words
        const unsigned int kFragmentModule[] = {
            0x07230203u, 0x00010000u, 0x0008000bu, 0x00000014u, 0x00000000u, 0x00020011u, 0x00000001u, 0x0006000bu,
            0x00000001u, 0x4c534c47u, 0x6474732eu, 0x3035342eu, 0x00000000u, 0x0003000eu, 0x00000000u, 0x00000001u,
            0x0006000fu, 0x00000004u, 0x00000004u, 0x6e69616du, 0x00000000u, 0x00000012u, 0x00030010u, 0x00000004u,
            0x00000008u, 0x00030003u, 0x00000002u, 0x000001c2u, 0x00040005u, 0x00000004u, 0x6e69616du, 0x00000000u,
            0x00030005u, 0x00000009u, 0x00000063u, 0x00050005u, 0x0000000eu, 0x61684375u, 0x6c656e6eu, 0x00000000u,
            0x00040005u, 0x00000012u, 0x6c6f436fu, 0x0000726fu, 0x00040047u, 0x0000000eu, 0x00000001u, 0x00000007u,
            0x00040047u, 0x00000012u, 0x0000001eu, 0x00000000u, 0x00020013u, 0x00000002u, 0x00030021u, 0x00000003u,
            0x00000002u, 0x00030016u, 0x00000006u, 0x00000020u, 0x00040017u, 0x00000007u, 0x00000006u, 0x00000004u,
            0x00040020u, 0x00000008u, 0x00000007u, 0x00000007u, 0x0004002bu, 0x00000006u, 0x0000000au, 0x00000000u,
            0x0004002bu, 0x00000006u, 0x0000000bu, 0x3f800000u, 0x0007002cu, 0x00000007u, 0x0000000cu, 0x0000000au,
            0x0000000au, 0x0000000au, 0x0000000bu, 0x00040015u, 0x0000000du, 0x00000020u, 0x00000001u, 0x00040032u,
            0x0000000du, 0x0000000eu, 0x00000000u, 0x00040020u, 0x0000000fu, 0x00000007u, 0x00000006u, 0x00040020u,
            0x00000011u, 0x00000003u, 0x00000007u, 0x0004003bu, 0x00000011u, 0x00000012u, 0x00000003u, 0x00050036u,
            0x00000002u, 0x00000004u, 0x00000000u, 0x00000003u, 0x000200f8u, 0x00000005u, 0x0004003bu, 0x00000008u,
            0x00000009u, 0x00000007u, 0x0003003eu, 0x00000009u, 0x0000000cu, 0x00050041u, 0x0000000fu, 0x00000010u,
            0x00000009u, 0x0000000eu, 0x0003003eu, 0x00000010u, 0x0000000bu, 0x0004003du, 0x00000007u, 0x00000013u,
            0x00000009u, 0x0003003eu, 0x00000012u, 0x00000013u, 0x000100fdu, 0x00010038u,
        };


        // The quad the vertex module transforms. Full-viewport before the scale, so a scale of
        // 0.5 covers exactly the middle half of each axis and the corners stay background.
        const float kQuad[] = {-1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f};

        // The specialization constant ids the two modules declare.
        constexpr unsigned int kScaleConstantId = 3;
        constexpr unsigned int kChannelConstantId = 7;

        unsigned int MakeSpirvShader(GLenum type, const unsigned int* words, size_t wordCount,
                                     unsigned int constantId, unsigned int constantValue, std::string* outLog) {
            const GLuint shader = glCreateShader(type);
            glShaderBinary(1, &shader, GL_SHADER_BINARY_FORMAT_SPIR_V, words,
                           static_cast<GLsizei>(wordCount * sizeof(unsigned int)));
            if (glGetError() != GL_NO_ERROR) {
                if (outLog) *outLog = "glShaderBinary rejected the module";
                glDeleteShader(shader);
                return 0;
            }

            GLint isSpirv = GL_FALSE;
            glGetShaderiv(shader, GL_SPIR_V_BINARY, &isSpirv);
            if (glGetError() != GL_NO_ERROR || isSpirv != GL_TRUE) {
                if (outLog) *outLog = "GL_SPIR_V_BINARY did not read TRUE after glShaderBinary";
                glDeleteShader(shader);
                return 0;
            }

            glSpecializeShader(shader, "main", 1, &constantId, &constantValue);
            GLint compiled = GL_FALSE;
            glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
            if (compiled != GL_TRUE) {
                if (outLog) {
                    GLint length = 0;
                    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
                    std::vector<char> log(static_cast<size_t>(length > 0 ? length : 1), '\0');
                    glGetShaderInfoLog(shader, static_cast<GLsizei>(log.size()), nullptr, log.data());
                    *outLog = std::string(log.data());
                }
                glDeleteShader(shader);
                return 0;
            }
            return shader;
        }

    } // namespace

    TEST_F(SpirvShaderBinaryScenario, ShaderBinaryFormatIsAdvertisedExactlyOnce) {
        if (!Ready()) return;

        GLint formatCount = -1;
        glGetIntegerv(GL_NUM_SHADER_BINARY_FORMATS, &formatCount);
        EXPECT_EQ(FirstGLError(), 0u);
        ASSERT_EQ(formatCount, 1) << "a 4.6 context supports exactly the SPIR-V shader binary format";

        std::vector<GLint> formats(static_cast<size_t>(formatCount), 0);
        glGetIntegerv(GL_SHADER_BINARY_FORMATS, formats.data());
        EXPECT_EQ(FirstGLError(), 0u);
        EXPECT_EQ(formats[0], static_cast<GLint>(GL_SHADER_BINARY_FORMAT_SPIR_V))
            << "the count and the list have to describe the same thing";
    }

    TEST_F(SpirvShaderBinaryScenario, AnUnsupportedBinaryFormatIsRejectedInsteadOfSilentlyAccepted) {
        if (!Ready()) return;

        const GLuint shader = glCreateShader(GL_VERTEX_SHADER);
        // 0x8DF9 is GL_SHADER_BINARY_FORMATS' neighbour, not a format: any value but
        // GL_SHADER_BINARY_FORMAT_SPIR_V is GL_INVALID_ENUM. The stub used to return silently.
        glShaderBinary(1, &shader, 0x8DF9, kVertexModule, sizeof(kVertexModule));
        EXPECT_EQ(FirstGLError(), static_cast<unsigned int>(GL_INVALID_ENUM));

        GLint isSpirv = GL_TRUE;
        glGetShaderiv(shader, GL_SPIR_V_BINARY, &isSpirv);
        EXPECT_EQ(FirstGLError(), 0u);
        EXPECT_EQ(isSpirv, GL_FALSE) << "a rejected glShaderBinary must not have attached anything";

        glDeleteShader(shader);
    }

    TEST_F(SpirvShaderBinaryScenario, CompileShaderOnASpirvShaderIsInvalidOperationAndShaderSourceTakesItBack) {
        if (!Ready()) return;

        const GLuint shader = glCreateShader(GL_VERTEX_SHADER);
        glShaderBinary(1, &shader, GL_SHADER_BINARY_FORMAT_SPIR_V, kVertexModule, sizeof(kVertexModule));
        ASSERT_EQ(FirstGLError(), 0u);

        glCompileShader(shader);
        EXPECT_EQ(FirstGLError(), static_cast<unsigned int>(GL_INVALID_OPERATION))
            << "glSpecializeShader, not glCompileShader, is what compiles a SPIR-V shader";

        // glShaderSource takes the object back to being a GLSL shader, and GL_SPIR_V_BINARY with
        // it - the transition the conformance suite checks explicitly.
        const char* source = "#version 450\nvoid main() { gl_Position = vec4(0.0); }\n";
        glShaderSource(shader, 1, &source, nullptr);
        ASSERT_EQ(FirstGLError(), 0u);
        GLint isSpirv = GL_TRUE;
        glGetShaderiv(shader, GL_SPIR_V_BINARY, &isSpirv);
        EXPECT_EQ(isSpirv, GL_FALSE);
        glCompileShader(shader);
        EXPECT_EQ(FirstGLError(), 0u) << "the object is an ordinary GLSL shader again";

        glDeleteShader(shader);
    }

    TEST_F(SpirvShaderBinaryScenario, SpecializeShaderErrorSurfaceMatchesTheExtension) {
        if (!Ready()) return;

        const GLuint shader = glCreateShader(GL_VERTEX_SHADER);
        glShaderBinary(1, &shader, GL_SHADER_BINARY_FORMAT_SPIR_V, kVertexModule, sizeof(kVertexModule));
        ASSERT_EQ(FirstGLError(), 0u);

        // 4242 is not one of the module's constant ids. ARB_gl_spirv enumerates that as
        // GL_INVALID_VALUE, and an erroring GL command has no other effect - so the shader is left
        // untouched rather than pushed into a failed-compile state.
        const unsigned int badId = 4242;
        const unsigned int value = 0;
        glSpecializeShader(shader, "main", 1, &badId, &value);
        EXPECT_EQ(FirstGLError(), static_cast<unsigned int>(GL_INVALID_VALUE));

        // Same for an entry point the module does not carry.
        glSpecializeShader(shader, "notMain", 0, nullptr, nullptr);
        EXPECT_EQ(FirstGLError(), static_cast<unsigned int>(GL_INVALID_VALUE));

        // Neither refusal specialized the shader, so a well-formed call still works.
        glSpecializeShader(shader, "main", 0, nullptr, nullptr);
        EXPECT_EQ(FirstGLError(), 0u);
        GLint compiled = GL_FALSE;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
        EXPECT_EQ(compiled, GL_TRUE);

        // But a SECOND specialization of a shader that HAS been specialized is INVALID_OPERATION
        // until glShaderBinary re-associates the module.
        glSpecializeShader(shader, "main", 0, nullptr, nullptr);
        EXPECT_EQ(FirstGLError(), static_cast<unsigned int>(GL_INVALID_OPERATION));
        glShaderBinary(1, &shader, GL_SHADER_BINARY_FORMAT_SPIR_V, kVertexModule, sizeof(kVertexModule));
        glSpecializeShader(shader, "main", 0, nullptr, nullptr);
        EXPECT_EQ(FirstGLError(), 0u) << "re-associating the module makes specialization legal again";

        glDeleteShader(shader);
    }

    TEST_F(SpirvShaderBinaryScenario, SpecializedModulesLinkAndRenderWithTheirConstantsApplied) {
        if (!Ready()) return;
        HeadlessGL& gl = Gl();
        const int width = gl.Width();
        const int height = gl.Height();
        ASSERT_GE(width, 16);
        ASSERT_GE(height, 16);

        std::string log;
        // Scale 0.5 as a float, handed over as the GLuint bit pattern the extension specifies.
        unsigned int halfBits = 0;
        const float half = 0.5f;
        std::memcpy(&halfBits, &half, sizeof(halfBits));

        const unsigned int vs = MakeSpirvShader(GL_VERTEX_SHADER, kVertexModule,
                                                sizeof(kVertexModule) / sizeof(kVertexModule[0]),
                                                kScaleConstantId, halfBits, &log);
        ASSERT_NE(vs, 0u) << "vertex: " << log;
        // Channel 1 is green; the module's own default is 0 (red), so a specialization that did
        // nothing paints the wrong colour.
        const unsigned int fs = MakeSpirvShader(GL_FRAGMENT_SHADER, kFragmentModule,
                                                sizeof(kFragmentModule) / sizeof(kFragmentModule[0]),
                                                kChannelConstantId, 1u, &log);
        ASSERT_NE(fs, 0u) << "fragment: " << log;

        const GLuint program = glCreateProgram();
        glAttachShader(program, vs);
        glAttachShader(program, fs);
        glLinkProgram(program);
        GLint linked = GL_FALSE;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        if (linked != GL_TRUE) {
            GLint length = 0;
            glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
            std::vector<char> programLog(static_cast<size_t>(length > 0 ? length : 1), '\0');
            glGetProgramInfoLog(program, static_cast<GLsizei>(programLog.size()), nullptr, programLog.data());
            FAIL() << "linking two specialized SPIR-V modules failed: " << programLog.data();
        }

        BindDefaultFramebuffer();
        glViewport(0, 0, width, height);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_DEPTH_TEST);
        ClearTo(0.0f, 0.0f, 0.0f, 1.0f);

        GLuint vao = 0, vbo = 0;
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        glGenBuffers(1, &vbo);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(kQuad), kQuad, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
        glUseProgram(program);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        EXPECT_EQ(FirstGLError(), 0u);

        const Image painted = ReadPixels(width, height);
        const Rgba8 centre = painted.At(width / 2, height / 2);
        EXPECT_LT(centre.r, 32) << "the fragment module wrote the wrong channel; constant id 7 was not applied";
        EXPECT_GT(centre.g, 224) << "the centre of a 0.5-scaled quad must be painted";

        // A pixel just inside the corner is OUTSIDE the 0.5-scaled quad and must still be the
        // clear colour - which is what proves constant id 3 reached the vertex module. At the
        // default scale of 1.0 the quad covers the whole viewport and this pixel would be green.
        const Rgba8 corner = painted.At(1, 1);
        EXPECT_LT(corner.g, 32) << "the quad was not scaled; the vertex specialization constant was not applied";

        glBindVertexArray(0);
        glDeleteBuffers(1, &vbo);
        glDeleteVertexArrays(1, &vao);
        glDeleteProgram(program);
        glDeleteShader(vs);
        glDeleteShader(fs);
        gl.EndFrame();
    }

} // namespace MGITest
