// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/NarrowIntegerAttributeScenario.cpp
// Copyright (c) 2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// 8/16-bit integer attribute data read through an int/uint input of the OTHER signedness.
//
// Every desktop GL driver hands such an input the source integer converted to 32 bits by the
// SOURCE type's signedness, then reinterpreted as the declared type: GL_UNSIGNED_BYTE 240 reaches
// an `int` input as 240, GL_BYTE -1 reaches a `uint` input as 0xFFFFFFFF. Sodium 0.5 relies on it
// (GL_UNSIGNED_BYTE lightmap data into `in ivec4 a_LightAndData`), and DirectVulkan once lost it:
// its fetch format has to carry the input's signedness, which re-extends narrow data by the
// input's rule, and every lightmap value from 128 up came out negative - dark terrain under
// shaderpacks. The values are captured with transform feedback so every bit is compared.
//
// The second case draws one program over two vertex arrays that differ only in the signedness
// of the data, in both orders and each twice in a row: the correction is chosen per draw from the
// data, so a pipeline or program cached for one array, or the variant the previous draw settled
// on, must never be handed to the other.

#include <cstddef>
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
        constexpr GLuint kPoison = 0xDEADBEEFu;

        GLuint CompileShader(GLenum type, const char* source, std::string* log) {
            const GLuint shader = glCreateShader(type);
            glShaderSource(shader, 1, &source, nullptr);
            glCompileShader(shader);
            GLint status = GL_FALSE;
            glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
            if (status == GL_FALSE) {
                GLint length = 0;
                glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
                std::vector<char> buffer(static_cast<std::size_t>(length) + 1, '\0');
                glGetShaderInfoLog(shader, length + 1, nullptr, buffer.data());
                if (log != nullptr) *log = buffer.data();
                glDeleteShader(shader);
                return 0;
            }
            return shader;
        }

        const char* const kFragmentSource = R"(#version 410 core
out vec4 color;
void main()
{
    color = vec4(1.0);
}
)";

        // A capture program: the vertex stage's flat outputs `varyings`, interleaved.
        GLuint BuildCaptureProgram(const char* vertexSource, const std::vector<const char*>& varyings,
                                   std::string* log) {
            const GLuint vertexShader = CompileShader(GL_VERTEX_SHADER, vertexSource, log);
            if (vertexShader == 0) return 0;
            const GLuint fragmentShader = CompileShader(GL_FRAGMENT_SHADER, kFragmentSource, log);
            if (fragmentShader == 0) {
                glDeleteShader(vertexShader);
                return 0;
            }
            const GLuint program = glCreateProgram();
            glAttachShader(program, vertexShader);
            glAttachShader(program, fragmentShader);
            glTransformFeedbackVaryings(program, static_cast<GLsizei>(varyings.size()), varyings.data(),
                                        GL_INTERLEAVED_ATTRIBS);
            glLinkProgram(program);
            glDeleteShader(vertexShader);
            glDeleteShader(fragmentShader);
            GLint status = GL_FALSE;
            glGetProgramiv(program, GL_LINK_STATUS, &status);
            if (status == GL_FALSE) {
                GLint length = 0;
                glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
                std::vector<char> buffer(static_cast<std::size_t>(length) + 1, '\0');
                glGetProgramInfoLog(program, length + 1, nullptr, buffer.data());
                if (log != nullptr) *log = buffer.data();
                glDeleteProgram(program);
                return 0;
            }
            return program;
        }

        // Every pairing the correction covers, plus two that must keep their value untouched:
        // matching signedness (location 2) and 32-bit data (location 6).
        const char* const kEveryPairingVertexSource = R"(#version 410 core
layout(location = 0) in ivec4 ubyteAsInt;
layout(location = 1) in uvec4 byteAsUint;
layout(location = 2) in uvec2 ushortAsUint;
layout(location = 3) in ivec2 ushortAsInt;
layout(location = 4) in uvec2 shortAsUint;
layout(location = 5) in ivec2 instancedUbyteAsInt;
layout(location = 6) in uint intAsUint;
flat out ivec4 capturedA;
flat out uvec4 capturedB;
flat out ivec4 capturedC;
flat out uvec4 capturedD;
void main()
{
    capturedA = ubyteAsInt;
    capturedB = byteAsUint;
    capturedC = ivec4(ushortAsInt, instancedUbyteAsInt);
    capturedD = uvec4(shortAsUint, ushortAsUint.x, intAsUint);
    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
}
)";

        const char* const kSingleInputVertexSource = R"(#version 410 core
layout(location = 0) in ivec4 value;
flat out ivec4 captured;
void main()
{
    captured = value;
    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
}
)";

        // One vertex of kEveryPairingVertexSource's per-vertex data, tightly packed.
        struct PerVertex {
            GLubyte ubyteAsInt[4];
            GLbyte byteAsUint[4];
            GLushort ushortAsUint[2];
            GLushort ushortAsInt[2];
            GLshort shortAsUint[2];
            GLint intAsUint;
        };

        class NarrowIntegerAttributeScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;
                glGenBuffers(1, &m_capture);
                glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, m_capture);
                glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, kCaptureWords * sizeof(GLuint), nullptr, GL_DYNAMIC_READ);
                glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, m_capture);
                ASSERT_EQ(glGetError(), GL_NO_ERROR) << "capture buffer setup";
            }

            void TearDown() override {
                if (!Ready()) return;
                glDisable(GL_RASTERIZER_DISCARD);
                glUseProgram(0);
                glBindVertexArray(0);
                for (const GLuint program : m_programs)
                    glDeleteProgram(program);
                for (const GLuint vao : m_vaos)
                    glDeleteVertexArrays(1, &vao);
                for (const GLuint buffer : m_buffers)
                    glDeleteBuffers(1, &buffer);
                if (m_capture != 0) glDeleteBuffers(1, &m_capture);
                ScenarioTest::TearDown();
            }

            GLuint MakeBuffer(const void* data, GLsizeiptr size) {
                GLuint buffer = 0;
                glGenBuffers(1, &buffer);
                glBindBuffer(GL_ARRAY_BUFFER, buffer);
                glBufferData(GL_ARRAY_BUFFER, size, data, GL_STATIC_DRAW);
                m_buffers.push_back(buffer);
                return buffer;
            }

            GLuint MakeVertexArray() {
                GLuint vao = 0;
                glGenVertexArrays(1, &vao);
                glBindVertexArray(vao);
                m_vaos.push_back(vao);
                return vao;
            }

            // One point drawn under GL_RASTERIZER_DISCARD; the first `words` captured words.
            std::vector<GLuint> Capture(GLuint program, GLuint vao, std::size_t words) {
                const std::vector<GLuint> poison(kCaptureWords, kPoison);
                glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, m_capture);
                glBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, kCaptureWords * sizeof(GLuint), poison.data());
                glUseProgram(program);
                glBindVertexArray(vao);
                glEnable(GL_RASTERIZER_DISCARD);
                glBeginTransformFeedback(GL_POINTS);
                glDrawArraysInstanced(GL_POINTS, 0, 1, 1);
                glEndTransformFeedback();
                glDisable(GL_RASTERIZER_DISCARD);
                std::vector<GLuint> result(words, kPoison);
                const void* mapped =
                    glMapBufferRange(GL_TRANSFORM_FEEDBACK_BUFFER, 0, words * sizeof(GLuint), GL_MAP_READ_BIT);
                if (mapped == nullptr) {
                    ADD_FAILURE() << "glMapBufferRange returned null";
                    return result;
                }
                std::memcpy(result.data(), mapped, words * sizeof(GLuint));
                glUnmapBuffer(GL_TRANSFORM_FEEDBACK_BUFFER);
                return result;
            }

            static constexpr std::size_t kCaptureWords = 16;
            GLuint m_capture = 0;
            std::vector<GLuint> m_programs;
            std::vector<GLuint> m_vaos;
            std::vector<GLuint> m_buffers;
        };

        TEST_F(NarrowIntegerAttributeScenario, EveryNarrowTypeKeepsItsOwnValueInAnInputOfTheOtherSignedness) {
            if (!Ready()) GTEST_SKIP();
            std::string log;
            const GLuint program = BuildCaptureProgram(kEveryPairingVertexSource,
                                                       {"capturedA", "capturedB", "capturedC", "capturedD"}, &log);
            ASSERT_NE(program, 0u) << log;
            m_programs.push_back(program);

            const PerVertex vertex{{240, 200, 128, 7}, {-1, 5, -128, 127}, {65000, 300},
                                   {65000, 40000},     {-2, 1000},         -1};
            const GLubyte instance[4] = {250, 129, 0, 0};
            const GLuint vao = MakeVertexArray();
            MakeBuffer(&vertex, sizeof(vertex));
            const auto offset = [](std::size_t bytes) { return reinterpret_cast<const void*>(bytes); };
            constexpr GLsizei stride = sizeof(PerVertex);
            glVertexAttribIPointer(0, 4, GL_UNSIGNED_BYTE, stride, offset(offsetof(PerVertex, ubyteAsInt)));
            glVertexAttribIPointer(1, 4, GL_BYTE, stride, offset(offsetof(PerVertex, byteAsUint)));
            glVertexAttribIPointer(2, 2, GL_UNSIGNED_SHORT, stride, offset(offsetof(PerVertex, ushortAsUint)));
            glVertexAttribIPointer(3, 2, GL_UNSIGNED_SHORT, stride, offset(offsetof(PerVertex, ushortAsInt)));
            glVertexAttribIPointer(4, 2, GL_SHORT, stride, offset(offsetof(PerVertex, shortAsUint)));
            glVertexAttribIPointer(6, 1, GL_INT, stride, offset(offsetof(PerVertex, intAsUint)));
            MakeBuffer(instance, sizeof(instance));
            glVertexAttribIPointer(5, 2, GL_UNSIGNED_BYTE, 4, nullptr);
            glVertexAttribDivisor(5, 1);
            for (GLuint location = 0; location <= 6; ++location)
                glEnableVertexAttribArray(location);
            ASSERT_EQ(glGetError(), GL_NO_ERROR) << "vertex array setup";

            const std::vector<GLuint> captured = Capture(program, vao, 16);
            const std::vector<GLuint> expected = {
                240u,        200u,   128u,        7u,   // GL_UNSIGNED_BYTE -> ivec4: zero-extended
                0xFFFFFFFFu, 5u,     0xFFFFFF80u, 127u, // GL_BYTE -> uvec4: sign-extended
                65000u,      40000u,                    // GL_UNSIGNED_SHORT -> ivec2: zero-extended
                250u,        129u,                      // instanced GL_UNSIGNED_BYTE -> ivec2
                0xFFFFFFFEu, 1000u,                     // GL_SHORT -> uvec2: sign-extended
                65000u,                                 // GL_UNSIGNED_SHORT -> uvec2: already right
                0xFFFFFFFFu,                            // 32-bit GL_INT -> uint: the same bits
            };
            EXPECT_EQ(captured, expected);
            EXPECT_EQ(glGetError(), GL_NO_ERROR);
        }

        TEST_F(NarrowIntegerAttributeScenario, OneProgramFollowsTheSignednessOfEachVertexArray) {
            if (!Ready()) GTEST_SKIP();
            std::string log;
            const GLuint program = BuildCaptureProgram(kSingleInputVertexSource, {"captured"}, &log);
            ASSERT_NE(program, 0u) << log;
            m_programs.push_back(program);

            // The same four bytes, once as GL_BYTE and once as GL_UNSIGNED_BYTE.
            const GLubyte bytes[4] = {0xFF, 0x80, 5, 127};
            const GLuint signedArray = MakeVertexArray();
            MakeBuffer(bytes, sizeof(bytes));
            glVertexAttribIPointer(0, 4, GL_BYTE, 0, nullptr);
            glEnableVertexAttribArray(0);
            const GLuint unsignedArray = MakeVertexArray();
            MakeBuffer(bytes, sizeof(bytes));
            glVertexAttribIPointer(0, 4, GL_UNSIGNED_BYTE, 0, nullptr);
            glEnableVertexAttribArray(0);
            ASSERT_EQ(glGetError(), GL_NO_ERROR) << "vertex array setup";

            const std::vector<GLuint> asSigned = {0xFFFFFFFFu, 0xFFFFFF80u, 5u, 127u};
            const std::vector<GLuint> asUnsigned = {255u, 128u, 5u, 127u};
            EXPECT_EQ(Capture(program, signedArray, 4), asSigned) << "GL_BYTE first";
            EXPECT_EQ(Capture(program, signedArray, 4), asSigned) << "GL_BYTE twice in a row";
            EXPECT_EQ(Capture(program, unsignedArray, 4), asUnsigned) << "then GL_UNSIGNED_BYTE";
            EXPECT_EQ(Capture(program, unsignedArray, 4), asUnsigned) << "GL_UNSIGNED_BYTE twice in a row";
            EXPECT_EQ(Capture(program, signedArray, 4), asSigned) << "GL_BYTE again";
            EXPECT_EQ(Capture(program, unsignedArray, 4), asUnsigned) << "GL_UNSIGNED_BYTE again";
            EXPECT_EQ(glGetError(), GL_NO_ERROR);
        }
    } // namespace
} // namespace MGITest
