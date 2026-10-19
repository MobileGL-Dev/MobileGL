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

// On DirectGLES every case also watches the driver itself (IntegerAttributeTypePeek.h): an array
// whose type has the other signedness from its input reads a value ES leaves undefined, so a case
// can show the right values on one driver and still render black on another. Where the watch
// cannot look it says so and the value checks still run.

#include <cstddef>
#include <cstring>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include "../Harness/HeadlessGL.h"
#include "../Harness/IntegerAttributeTypePeek.h"
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

        // A program of `vertexSource` and `fragmentSource` capturing the vertex stage's flat outputs
        // `varyings` (none: no capture), interleaved; `beforeLink` runs on it just before the link.
        GLuint BuildProgram(const char* vertexSource, const char* fragmentSource,
                            const std::vector<const char*>& varyings, std::string* log,
                            const std::function<void(GLuint)>& beforeLink = {}) {
            const GLuint vertexShader = CompileShader(GL_VERTEX_SHADER, vertexSource, log);
            if (vertexShader == 0) return 0;
            const GLuint fragmentShader = CompileShader(GL_FRAGMENT_SHADER, fragmentSource, log);
            if (fragmentShader == 0) {
                glDeleteShader(vertexShader);
                return 0;
            }
            const GLuint program = glCreateProgram();
            glAttachShader(program, vertexShader);
            glAttachShader(program, fragmentShader);
            if (!varyings.empty()) {
                glTransformFeedbackVaryings(program, static_cast<GLsizei>(varyings.size()), varyings.data(),
                                            GL_INTERLEAVED_ATTRIBS);
            }
            if (beforeLink) beforeLink(program);
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

        // A capture program: the vertex stage's flat outputs `varyings`, interleaved.
        GLuint BuildCaptureProgram(const char* vertexSource, const std::vector<const char*>& varyings, std::string* log,
                                   const std::function<void(GLuint)>& beforeLink = {}) {
            return BuildProgram(vertexSource, kFragmentSource, varyings, log, beforeLink);
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

        const char* const kSingleUintInputVertexSource = R"(#version 410 core
layout(location = 0) in uvec4 value;
flat out uvec4 captured;
void main()
{
    captured = value;
    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
}
)";

        // No layout qualifier: the location comes from glBindAttribLocation.
        const char* const kUnplacedInputVertexSource = R"(#version 410 core
in ivec4 value;
flat out ivec4 captured;
void main()
{
    captured = value;
    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
}
)";

        const char* const kWideAndMatchedVertexSource = R"(#version 410 core
layout(location = 0) in ivec2 uintAsInt;
layout(location = 1) in uvec2 ushortAsUint;
flat out ivec4 captured;
void main()
{
    captured = ivec4(uintAsInt, ivec2(ushortAsUint));
    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
}
)";

        // A program without a capture list, read back through a 2x1 RGBA32I target instead, so its
        // first build is not one that captures. Vertex i lands on pixel i.
        const char* const kRenderVertexSource = R"(#version 410 core
layout(location = 0) in ivec4 value;
flat out ivec4 passed;
void main()
{
    passed = value;
    gl_Position = vec4(gl_VertexID == 0 ? -0.5 : 0.5, 0.0, 0.0, 1.0);
}
)";

        const char* const kRenderFragmentSource = R"(#version 410 core
flat in ivec4 passed;
layout(location = 0) out ivec4 color;
void main()
{
    color = passed;
}
)";

        const char* const kSeparableVertexSource = R"(#version 410 core
layout(location = 0) in ivec4 value;
layout(location = 0) flat out ivec4 passed;
out gl_PerVertex { vec4 gl_Position; };
void main()
{
    passed = value;
    gl_Position = vec4(gl_VertexID == 0 ? -0.5 : 0.5, 0.0, 0.0, 1.0);
}
)";

        const char* const kSeparableFragmentSource = R"(#version 410 core
layout(location = 0) flat in ivec4 passed;
layout(location = 0) out ivec4 color;
void main()
{
    color = passed;
}
)";

        // The same four bytes as GL_BYTE and as GL_UNSIGNED_BYTE, and the words each reaches an
        // input with: sign-extended from GL_BYTE, zero-extended from GL_UNSIGNED_BYTE, whatever the
        // input's own signedness.
        const GLubyte kBytes[4] = {0xFF, 0x80, 5, 127};
        const std::vector<GLuint> kBytesAsSigned = {0xFFFFFFFFu, 0xFFFFFF80u, 5u, 127u};
        const std::vector<GLuint> kBytesAsUnsigned = {255u, 128u, 5u, 127u};
        const GLubyte kLightBytes[4] = {240, 200, 128, 7};
        const std::vector<GLuint> kLightWords = {240u, 200u, 128u, 7u};

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
                std::string whyNot;
                m_watching = StartIntegerAttributeTypeWatch(&whyNot);
                if (!m_watching) {
                    // A log line and a property, never a skip: the value checks below still run.
                    std::cout << "[itest] integer attribute type watch off: " << whyNot << std::endl;
                    RecordProperty("integer_attribute_type_watch", whyNot);
                }
            }

            void TearDown() override {
                if (!Ready()) return;
                if (m_watching) StopIntegerAttributeTypeWatch();
                if (m_target != 0) glDeleteFramebuffers(1, &m_target);
                if (m_targetTexture != 0) glDeleteTextures(1, &m_targetTexture);
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
            std::vector<GLuint> Capture(GLuint program, GLuint vao, std::size_t words,
                                        const std::function<void()>& draw = {}) {
                const std::vector<GLuint> poison(kCaptureWords, kPoison);
                glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, m_capture);
                glBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, kCaptureWords * sizeof(GLuint), poison.data());
                glUseProgram(program);
                glBindVertexArray(vao);
                glEnable(GL_RASTERIZER_DISCARD);
                glBeginTransformFeedback(GL_POINTS);
                if (draw)
                    draw();
                else
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

            // `points` points drawn into a 2x1 RGBA32I target with `program` (0: the bound pipeline),
            // by `draw` or one glDrawArrays; the four words its fragment stage wrote for each.
            std::vector<GLuint> Render(GLuint program, GLuint vao, int points = 1,
                                       const std::function<void()>& draw = {}) {
                if (m_target == 0) {
                    glGenTextures(1, &m_targetTexture);
                    glBindTexture(GL_TEXTURE_2D, m_targetTexture);
                    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA32I, 2, 1);
                    glBindTexture(GL_TEXTURE_2D, 0);
                    glGenFramebuffers(1, &m_target);
                    glBindFramebuffer(GL_FRAMEBUFFER, m_target);
                    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_targetTexture, 0);
                    EXPECT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));
                }
                glBindFramebuffer(GL_FRAMEBUFFER, m_target);
                glViewport(0, 0, 2, 1);
                const GLint poison[4] = {static_cast<GLint>(kPoison), static_cast<GLint>(kPoison),
                                         static_cast<GLint>(kPoison), static_cast<GLint>(kPoison)};
                glClearBufferiv(GL_COLOR, 0, poison);
                glUseProgram(program);
                glBindVertexArray(vao);
                if (draw)
                    draw();
                else
                    glDrawArrays(GL_POINTS, 0, 1);
                GLint pixels[8] = {};
                glReadPixels(0, 0, 2, 1, GL_RGBA_INTEGER, GL_INT, pixels);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                return std::vector<GLuint>(pixels, pixels + 4 * points);
            }

            // A vertex array whose location 0 reads `bytes` as four `type` components.
            GLuint MakeByteArray(const GLubyte (&bytes)[4], GLenum type) {
                const GLuint vao = MakeVertexArray();
                MakeBuffer(bytes, sizeof(bytes));
                glVertexAttribIPointer(0, 4, type, 0, nullptr);
                glEnableVertexAttribArray(0);
                return vao;
            }

            // The driver's view of every draw so far; null where the watch is off.
            bool ReadWatch(IntegerAttributeTypeTally* tally) {
                if (!m_watching) return false;
                glFinish();
                const bool read = ReadIntegerAttributeTypeWatch(tally);
                EXPECT_TRUE(read) << "the integer attribute type watch started but could not be read";
                return read;
            }

            // Every array a draw so far handed the driver has the signedness of the input it feeds.
            void ExpectDriverArraysFitTheirInputs() {
                IntegerAttributeTypeTally tally;
                if (!ReadWatch(&tally)) return;
                RecordProperty("integer_attribute_type_watch_draws", static_cast<int>(tally.draws));
                EXPECT_GT(tally.draws, 0u) << "the watch saw no draw, so it checked nothing";
                EXPECT_EQ(tally.violations, 0u) << tally.firstViolation;
            }

            // Driver links so far, or -1 where the watch is off.
            long LinksSoFar() {
                IntegerAttributeTypeTally tally;
                return ReadWatch(&tally) ? static_cast<long>(tally.links) : -1;
            }

            static constexpr std::size_t kCaptureWords = 16;
            bool m_watching = false;
            GLuint m_target = 0;
            GLuint m_targetTexture = 0;
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
            ExpectDriverArraysFitTheirInputs();
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
            ExpectDriverArraysFitTheirInputs();
        }

        // The negative control for the driver watch, on canned input: the rule must call an
        // unsigned array into an int input wrong, and every matching pairing right.
        TEST_F(NarrowIntegerAttributeScenario, TheDriverWatchRuleRejectsAnArrayOfTheOtherSignedness) {
            EXPECT_FALSE(IntegerAttributeTypeFitsInput(GL_INT_VEC4, true, GL_UNSIGNED_BYTE));
            EXPECT_FALSE(IntegerAttributeTypeFitsInput(GL_UNSIGNED_INT_VEC2, true, GL_SHORT));
            EXPECT_FALSE(IntegerAttributeTypeFitsInput(GL_INT, true, GL_UNSIGNED_INT));
            EXPECT_FALSE(IntegerAttributeTypeFitsInput(GL_INT_VEC4, false, GL_FLOAT));
            EXPECT_FALSE(IntegerAttributeTypeFitsInput(GL_FLOAT_VEC4, true, GL_BYTE));
            EXPECT_TRUE(IntegerAttributeTypeFitsInput(GL_INT_VEC4, true, GL_BYTE));
            EXPECT_TRUE(IntegerAttributeTypeFitsInput(GL_INT_VEC2, true, GL_SHORT));
            EXPECT_TRUE(IntegerAttributeTypeFitsInput(GL_INT, true, GL_INT));
            EXPECT_TRUE(IntegerAttributeTypeFitsInput(GL_UNSIGNED_INT_VEC4, true, GL_UNSIGNED_BYTE));
            EXPECT_TRUE(IntegerAttributeTypeFitsInput(GL_UNSIGNED_INT_VEC3, true, GL_UNSIGNED_SHORT));
            EXPECT_TRUE(IntegerAttributeTypeFitsInput(GL_UNSIGNED_INT, true, GL_UNSIGNED_INT));
            EXPECT_TRUE(IntegerAttributeTypeFitsInput(GL_FLOAT_VEC4, false, GL_UNSIGNED_BYTE));
        }

        // One array read in turn by an int and a uint program: each switch changes which of the two
        // signednesses the array has to be issued with, and the second round must need no relink.
        TEST_F(NarrowIntegerAttributeScenario, AnIntAndAUintProgramTakeTurnsOnOneArray) {
            if (!Ready()) GTEST_SKIP();
            std::string log;
            const GLuint intProgram = BuildCaptureProgram(kSingleInputVertexSource, {"captured"}, &log);
            ASSERT_NE(intProgram, 0u) << log;
            m_programs.push_back(intProgram);
            const GLuint uintProgram = BuildCaptureProgram(kSingleUintInputVertexSource, {"captured"}, &log);
            ASSERT_NE(uintProgram, 0u) << log;
            m_programs.push_back(uintProgram);
            const GLuint signedArray = MakeByteArray(kBytes, GL_BYTE);
            ASSERT_EQ(glGetError(), GL_NO_ERROR) << "vertex array setup";

            EXPECT_EQ(Capture(intProgram, signedArray, 4), kBytesAsSigned) << "int first";
            EXPECT_EQ(Capture(uintProgram, signedArray, 4), kBytesAsSigned) << "then uint";
            const long linksAfterFirstRound = LinksSoFar();
            for (int round = 0; round < 2; ++round) {
                EXPECT_EQ(Capture(intProgram, signedArray, 4), kBytesAsSigned) << "int, round " << round;
                EXPECT_EQ(Capture(uintProgram, signedArray, 4), kBytesAsSigned) << "uint, round " << round;
            }
            EXPECT_EQ(LinksSoFar(), linksAfterFirstRound) << "switching programs relinked one";
            EXPECT_EQ(glGetError(), GL_NO_ERROR);
            ExpectDriverArraysFitTheirInputs();
        }

        // A program that never captures is built for the data it first meets. Fed data of the other
        // signedness afterwards it may be rebuilt once, and every value - before, during and after
        // the switch - is still GL's.
        TEST_F(NarrowIntegerAttributeScenario, AProgramFirstDrawnWithMatchingDataKeepsEveryValue) {
            if (!Ready()) GTEST_SKIP();
            std::string log;
            const GLuint program = BuildProgram(kRenderVertexSource, kRenderFragmentSource, {}, &log);
            ASSERT_NE(program, 0u) << log;
            m_programs.push_back(program);
            const GLuint signedArray = MakeByteArray(kBytes, GL_BYTE);
            const GLuint unsignedArray = MakeByteArray(kBytes, GL_UNSIGNED_BYTE);
            ASSERT_EQ(glGetError(), GL_NO_ERROR) << "vertex array setup";

            EXPECT_EQ(Render(program, signedArray), kBytesAsSigned) << "matching data first";
            const long linksAfterFirstDraw = LinksSoFar();
            EXPECT_EQ(Render(program, unsignedArray), kBytesAsUnsigned) << "then the other signedness";
            EXPECT_EQ(Render(program, signedArray), kBytesAsSigned) << "matching data again";
            EXPECT_EQ(Render(program, unsignedArray), kBytesAsUnsigned) << "the other signedness again";
            if (linksAfterFirstDraw >= 0) EXPECT_LE(LinksSoFar(), linksAfterFirstDraw + 1) << "rebuilt more than once";
            EXPECT_EQ(glGetError(), GL_NO_ERROR);
            ExpectDriverArraysFitTheirInputs();
        }

        // Two programs from one source, the first built for matching data only and the second for
        // data of the other signedness. Whatever the backend shares between the two builds must not
        // hand the second one the first one's shader.
        TEST_F(NarrowIntegerAttributeScenario, TwoProgramsFromOneSourceKeepTheirOwnBuilds) {
            if (!Ready()) GTEST_SKIP();
            std::string log;
            const GLuint first = BuildProgram(kRenderVertexSource, kRenderFragmentSource, {}, &log);
            ASSERT_NE(first, 0u) << log;
            m_programs.push_back(first);
            const GLuint second = BuildProgram(kRenderVertexSource, kRenderFragmentSource, {}, &log);
            ASSERT_NE(second, 0u) << log;
            m_programs.push_back(second);
            const GLuint signedArray = MakeByteArray(kBytes, GL_BYTE);
            const GLuint unsignedArray = MakeByteArray(kBytes, GL_UNSIGNED_BYTE);
            ASSERT_EQ(glGetError(), GL_NO_ERROR) << "vertex array setup";

            EXPECT_EQ(Render(first, signedArray), kBytesAsSigned) << "the first program, matching data";
            EXPECT_EQ(Render(second, unsignedArray), kBytesAsUnsigned) << "the second, the other signedness";
            EXPECT_EQ(Render(first, signedArray), kBytesAsSigned) << "the first again";
            EXPECT_EQ(glGetError(), GL_NO_ERROR);
            ExpectDriverArraysFitTheirInputs();
        }

        // A relink between two draws of the same data: whatever per-program state the correction
        // keeps has to survive the program being rebuilt underneath it.
        TEST_F(NarrowIntegerAttributeScenario, ARelinkBetweenDrawsKeepsTheValues) {
            if (!Ready()) GTEST_SKIP();
            std::string log;
            const GLuint program = BuildCaptureProgram(kSingleInputVertexSource, {"captured"}, &log);
            ASSERT_NE(program, 0u) << log;
            m_programs.push_back(program);
            const GLuint unsignedArray = MakeByteArray(kLightBytes, GL_UNSIGNED_BYTE);
            ASSERT_EQ(glGetError(), GL_NO_ERROR) << "vertex array setup";

            EXPECT_EQ(Capture(program, unsignedArray, 4), kLightWords) << "before the relink";
            glLinkProgram(program);
            GLint status = GL_FALSE;
            glGetProgramiv(program, GL_LINK_STATUS, &status);
            ASSERT_EQ(status, GL_TRUE);
            EXPECT_EQ(Capture(program, unsignedArray, 4), kLightWords) << "after the relink";
            EXPECT_EQ(Capture(program, unsignedArray, 4), kLightWords) << "and once more";
            EXPECT_EQ(glGetError(), GL_NO_ERROR);
            ExpectDriverArraysFitTheirInputs();
        }

        // A program and a vertex array deleted and created again, most likely under the same names,
        // with the signedness of input and data swapped: nothing learned about the first pair may
        // be believed for the second.
        TEST_F(NarrowIntegerAttributeScenario, AProgramAndAnArrayCreatedAgainStartOver) {
            if (!Ready()) GTEST_SKIP();
            std::string log;
            const GLuint intProgram = BuildCaptureProgram(kSingleInputVertexSource, {"captured"}, &log);
            ASSERT_NE(intProgram, 0u) << log;
            GLuint unsignedArray = 0;
            glGenVertexArrays(1, &unsignedArray);
            glBindVertexArray(unsignedArray);
            MakeBuffer(kLightBytes, sizeof(kLightBytes));
            glVertexAttribIPointer(0, 4, GL_UNSIGNED_BYTE, 0, nullptr);
            glEnableVertexAttribArray(0);
            ASSERT_EQ(glGetError(), GL_NO_ERROR) << "first pair setup";
            EXPECT_EQ(Capture(intProgram, unsignedArray, 4), kLightWords) << "the first pair";
            glUseProgram(0);
            glBindVertexArray(0);
            glDeleteProgram(intProgram);
            glDeleteVertexArrays(1, &unsignedArray);

            const GLuint uintProgram = BuildCaptureProgram(kSingleUintInputVertexSource, {"captured"}, &log);
            ASSERT_NE(uintProgram, 0u) << log;
            m_programs.push_back(uintProgram);
            const GLuint signedArray = MakeByteArray(kBytes, GL_BYTE);
            ASSERT_EQ(glGetError(), GL_NO_ERROR) << "second pair setup";
            // The case is about the reused names; a fresh name would check nothing it is for.
            EXPECT_EQ(uintProgram, intProgram) << "the program name was not reused";
            EXPECT_EQ(signedArray, unsignedArray) << "the vertex array name was not reused";
            EXPECT_EQ(Capture(uintProgram, signedArray, 4), kBytesAsSigned) << "the second pair";
            EXPECT_EQ(glGetError(), GL_NO_ERROR);
            ExpectDriverArraysFitTheirInputs();
        }

        // The input's location comes from glBindAttribLocation rather than the shader.
        TEST_F(NarrowIntegerAttributeScenario, ABoundAttributeLocationIsCorrected) {
            if (!Ready()) GTEST_SKIP();
            std::string log;
            const GLuint program = BuildCaptureProgram(kUnplacedInputVertexSource, {"captured"}, &log,
                                                       [](GLuint p) { glBindAttribLocation(p, 5, "value"); });
            ASSERT_NE(program, 0u) << log;
            m_programs.push_back(program);
            ASSERT_EQ(glGetAttribLocation(program, "value"), 5);
            const GLuint vao = MakeVertexArray();
            MakeBuffer(kLightBytes, sizeof(kLightBytes));
            glVertexAttribIPointer(5, 4, GL_UNSIGNED_BYTE, 0, nullptr);
            glEnableVertexAttribArray(5);
            ASSERT_EQ(glGetError(), GL_NO_ERROR) << "vertex array setup";

            EXPECT_EQ(Capture(program, vao, 4), kLightWords);
            EXPECT_EQ(glGetError(), GL_NO_ERROR);
            ExpectDriverArraysFitTheirInputs();
        }

        // The array is described through the direct-state-access entry points.
        TEST_F(NarrowIntegerAttributeScenario, ADirectStateAccessArrayIsCorrected) {
            if (!Ready()) GTEST_SKIP();
            std::string log;
            const GLuint program = BuildCaptureProgram(kSingleInputVertexSource, {"captured"}, &log);
            ASSERT_NE(program, 0u) << log;
            m_programs.push_back(program);
            GLuint vao = 0;
            glCreateVertexArrays(1, &vao);
            m_vaos.push_back(vao);
            GLuint buffer = 0;
            glCreateBuffers(1, &buffer);
            m_buffers.push_back(buffer);
            glNamedBufferData(buffer, sizeof(kLightBytes), kLightBytes, GL_STATIC_DRAW);
            glVertexArrayVertexBuffer(vao, 0, buffer, 0, 4);
            glVertexArrayAttribIFormat(vao, 0, 4, GL_UNSIGNED_BYTE, 0);
            glVertexArrayAttribBinding(vao, 0, 0);
            glEnableVertexArrayAttrib(vao, 0);
            ASSERT_EQ(glGetError(), GL_NO_ERROR) << "vertex array setup";

            EXPECT_EQ(Capture(program, vao, 4), kLightWords);
            EXPECT_EQ(glGetError(), GL_NO_ERROR);
            ExpectDriverArraysFitTheirInputs();
        }

        // A binding of stride zero, which never advances: two points read the same element.
        TEST_F(NarrowIntegerAttributeScenario, AZeroStrideBindingIsCorrected) {
            if (!Ready()) GTEST_SKIP();
            std::string log;
            const GLuint program = BuildCaptureProgram(kSingleInputVertexSource, {"captured"}, &log);
            ASSERT_NE(program, 0u) << log;
            m_programs.push_back(program);
            const GLuint vao = MakeVertexArray();
            const GLuint buffer = MakeBuffer(kLightBytes, sizeof(kLightBytes));
            glVertexAttribIFormat(0, 4, GL_UNSIGNED_BYTE, 0);
            glVertexAttribBinding(0, 0);
            glBindVertexBuffer(0, buffer, 0, 0);
            glEnableVertexAttribArray(0);
            ASSERT_EQ(glGetError(), GL_NO_ERROR) << "vertex array setup";

            std::vector<GLuint> expected = kLightWords;
            expected.insert(expected.end(), kLightWords.begin(), kLightWords.end());
            EXPECT_EQ(Capture(program, vao, 8, [] { glDrawArrays(GL_POINTS, 0, 2); }), expected);
            EXPECT_EQ(glGetError(), GL_NO_ERROR);
            ExpectDriverArraysFitTheirInputs();
        }

        // An array in the application's own memory, as ClientVertexArrayScenario draws one on
        // every arm.
        TEST_F(NarrowIntegerAttributeScenario, AClientMemoryArrayIsCorrected) {
            if (!Ready()) GTEST_SKIP();
            std::string log;
            const GLuint program = BuildCaptureProgram(kSingleInputVertexSource, {"captured"}, &log);
            ASSERT_NE(program, 0u) << log;
            m_programs.push_back(program);
            const GLuint vao = MakeVertexArray();
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            glVertexAttribIPointer(0, 4, GL_UNSIGNED_BYTE, 0, kLightBytes);
            glEnableVertexAttribArray(0);
            ASSERT_EQ(glGetError(), GL_NO_ERROR) << "vertex array setup";

            EXPECT_EQ(Capture(program, vao, 4, [] { glDrawArrays(GL_POINTS, 0, 1); }), kLightWords);
            EXPECT_EQ(glGetError(), GL_NO_ERROR);
            ExpectDriverArraysFitTheirInputs();
        }

        // 32-bit data of the other signedness is the same bits, and 16-bit data of the input's own
        // signedness is untouched; both still reach the driver with the input's signedness.
        TEST_F(NarrowIntegerAttributeScenario, WideDataAndMatchingDataKeepTheirBits) {
            if (!Ready()) GTEST_SKIP();
            std::string log;
            const GLuint program = BuildCaptureProgram(kWideAndMatchedVertexSource, {"captured"}, &log);
            ASSERT_NE(program, 0u) << log;
            m_programs.push_back(program);
            const GLuint wide[2] = {0xFFFFFFF0u, 7u};
            const GLushort matched[2] = {65000, 300};
            const GLuint vao = MakeVertexArray();
            MakeBuffer(wide, sizeof(wide));
            glVertexAttribIPointer(0, 2, GL_UNSIGNED_INT, 0, nullptr);
            MakeBuffer(matched, sizeof(matched));
            glVertexAttribIPointer(1, 2, GL_UNSIGNED_SHORT, 0, nullptr);
            glEnableVertexAttribArray(0);
            glEnableVertexAttribArray(1);
            ASSERT_EQ(glGetError(), GL_NO_ERROR) << "vertex array setup";

            const std::vector<GLuint> expected = {0xFFFFFFF0u, 7u, 65000u, 300u};
            EXPECT_EQ(Capture(program, vao, 4), expected);
            EXPECT_EQ(glGetError(), GL_NO_ERROR);
            ExpectDriverArraysFitTheirInputs();
        }

        // Two points through one glMultiDrawArrays, read back rather than captured.
        TEST_F(NarrowIntegerAttributeScenario, AMultiDrawIsCorrected) {
            if (!Ready()) GTEST_SKIP();
            std::string log;
            const GLuint program = BuildProgram(kRenderVertexSource, kRenderFragmentSource, {}, &log);
            ASSERT_NE(program, 0u) << log;
            m_programs.push_back(program);
            const GLubyte points[8] = {240, 200, 128, 7, 250, 129, 1, 255};
            const GLuint vao = MakeVertexArray();
            MakeBuffer(points, sizeof(points));
            glVertexAttribIPointer(0, 4, GL_UNSIGNED_BYTE, 0, nullptr);
            glEnableVertexAttribArray(0);
            ASSERT_EQ(glGetError(), GL_NO_ERROR) << "vertex array setup";

            const std::vector<GLuint> expected = {240u, 200u, 128u, 7u, 250u, 129u, 1u, 255u};
            EXPECT_EQ(Render(program, vao, 2,
                             [] {
                                 const GLint firsts[2] = {0, 1};
                                 const GLsizei counts[2] = {1, 1};
                                 glMultiDrawArrays(GL_POINTS, firsts, counts, 2);
                             }),
                      expected);
            EXPECT_EQ(glGetError(), GL_NO_ERROR);
            ExpectDriverArraysFitTheirInputs();
        }

        // The integer input belongs to a separable vertex program bound through a pipeline.
        TEST_F(NarrowIntegerAttributeScenario, AProgramPipelineVertexStageIsCorrected) {
            if (!Ready()) GTEST_SKIP();
            const GLuint vertexStage = glCreateShaderProgramv(GL_VERTEX_SHADER, 1, &kSeparableVertexSource);
            const GLuint fragmentStage = glCreateShaderProgramv(GL_FRAGMENT_SHADER, 1, &kSeparableFragmentSource);
            ASSERT_NE(vertexStage, 0u);
            ASSERT_NE(fragmentStage, 0u);
            m_programs.push_back(vertexStage);
            m_programs.push_back(fragmentStage);
            GLint linked = GL_FALSE;
            glGetProgramiv(vertexStage, GL_LINK_STATUS, &linked);
            ASSERT_EQ(linked, GL_TRUE);
            glGetProgramiv(fragmentStage, GL_LINK_STATUS, &linked);
            ASSERT_EQ(linked, GL_TRUE);
            GLuint pipeline = 0;
            glGenProgramPipelines(1, &pipeline);
            glUseProgramStages(pipeline, GL_VERTEX_SHADER_BIT, vertexStage);
            glUseProgramStages(pipeline, GL_FRAGMENT_SHADER_BIT, fragmentStage);
            const GLuint unsignedArray = MakeByteArray(kLightBytes, GL_UNSIGNED_BYTE);
            ASSERT_EQ(glGetError(), GL_NO_ERROR) << "pipeline setup";

            glUseProgram(0);
            glBindProgramPipeline(pipeline);
            EXPECT_EQ(Render(0, unsignedArray), kLightWords);
            glBindProgramPipeline(0);
            glDeleteProgramPipelines(1, &pipeline);
            EXPECT_EQ(glGetError(), GL_NO_ERROR);
            ExpectDriverArraysFitTheirInputs();
        }

        // Whatever the driver is handed, the application still reads back the type it declared.
        TEST_F(NarrowIntegerAttributeScenario, TheArrayTypeQueryStillReportsTheDeclaredType) {
            if (!Ready()) GTEST_SKIP();
            std::string log;
            const GLuint program = BuildCaptureProgram(kSingleInputVertexSource, {"captured"}, &log);
            ASSERT_NE(program, 0u) << log;
            m_programs.push_back(program);
            const GLuint unsignedArray = MakeByteArray(kLightBytes, GL_UNSIGNED_BYTE);
            ASSERT_EQ(glGetError(), GL_NO_ERROR) << "vertex array setup";

            EXPECT_EQ(Capture(program, unsignedArray, 4), kLightWords);
            glBindVertexArray(unsignedArray);
            GLint type = 0;
            GLint integer = 0;
            glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_TYPE, &type);
            glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_INTEGER, &integer);
            EXPECT_EQ(type, GL_UNSIGNED_BYTE);
            EXPECT_EQ(integer, GL_TRUE);
            EXPECT_EQ(glGetError(), GL_NO_ERROR);
            ExpectDriverArraysFitTheirInputs();
        }

        // A capture program's first draws, matching data then the other signedness, inside one
        // capture: the program has to be built for both before the capture opens, since nothing
        // may relink it while it captures.
        TEST_F(NarrowIntegerAttributeScenario, ACaptureProgramMeetsBothSignednessesInOneCapture) {
            if (!Ready()) GTEST_SKIP();
            const long linksBefore = LinksSoFar();
            std::string log;
            const GLuint program = BuildCaptureProgram(kSingleInputVertexSource, {"captured"}, &log);
            ASSERT_NE(program, 0u) << log;
            m_programs.push_back(program);
            const GLuint signedArray = MakeByteArray(kBytes, GL_BYTE);
            const GLuint unsignedArray = MakeByteArray(kLightBytes, GL_UNSIGNED_BYTE);
            ASSERT_EQ(glGetError(), GL_NO_ERROR) << "vertex array setup";

            std::vector<GLuint> expected = kBytesAsSigned;
            expected.insert(expected.end(), kLightWords.begin(), kLightWords.end());
            EXPECT_EQ(Capture(program, signedArray, 8,
                              [unsignedArray] {
                                  glDrawArrays(GL_POINTS, 0, 1);
                                  glBindVertexArray(unsignedArray);
                                  glDrawArrays(GL_POINTS, 0, 1);
                              }),
                      expected);
            if (linksBefore >= 0) EXPECT_LE(LinksSoFar(), linksBefore + 1) << "the program was relinked";
            EXPECT_EQ(glGetError(), GL_NO_ERROR);
            ExpectDriverArraysFitTheirInputs();
        }

        // The application relinks the same program object with its input at location 0 turned
        // from ivec4 into uvec4: what was known about the old link's inputs no longer holds.
        TEST_F(NarrowIntegerAttributeScenario, ARelinkThatChangesTheInputsSignednessStartsOver) {
            if (!Ready()) GTEST_SKIP();
            std::string log;
            const GLuint program = BuildCaptureProgram(kSingleInputVertexSource, {"captured"}, &log);
            ASSERT_NE(program, 0u) << log;
            m_programs.push_back(program);
            const GLuint unsignedArray = MakeByteArray(kLightBytes, GL_UNSIGNED_BYTE);
            const GLuint signedArray = MakeByteArray(kBytes, GL_BYTE);
            ASSERT_EQ(glGetError(), GL_NO_ERROR) << "vertex array setup";
            EXPECT_EQ(Capture(program, unsignedArray, 4), kLightWords) << "the ivec4 link";

            GLuint attached[4] = {};
            GLsizei attachedCount = 0;
            glGetAttachedShaders(program, 4, &attachedCount, attached);
            for (GLsizei i = 0; i < attachedCount; ++i)
                glDetachShader(program, attached[i]);
            const GLuint vertexShader = CompileShader(GL_VERTEX_SHADER, kSingleUintInputVertexSource, &log);
            ASSERT_NE(vertexShader, 0u) << log;
            const GLuint fragmentShader = CompileShader(GL_FRAGMENT_SHADER, kFragmentSource, &log);
            ASSERT_NE(fragmentShader, 0u) << log;
            glAttachShader(program, vertexShader);
            glAttachShader(program, fragmentShader);
            glLinkProgram(program);
            glDeleteShader(vertexShader);
            glDeleteShader(fragmentShader);
            GLint status = GL_FALSE;
            glGetProgramiv(program, GL_LINK_STATUS, &status);
            ASSERT_EQ(status, GL_TRUE);

            EXPECT_EQ(Capture(program, signedArray, 4), kBytesAsSigned) << "the uvec4 link, GL_BYTE data";
            EXPECT_EQ(Capture(program, unsignedArray, 4), kLightWords) << "the uvec4 link, matching data";
            EXPECT_EQ(glGetError(), GL_NO_ERROR);
            ExpectDriverArraysFitTheirInputs();
        }
    } // namespace
} // namespace MGITest
