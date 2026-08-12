// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/VertexAttribBindingScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// ARB_vertex_attrib_binding: the separate format/binding state the GL 4.3 vertex
// input model is made of, read back out of the draw that consumed it.
//
// Every scenario here captures the vertex shader's inputs with transform feedback
// under GL_RASTERIZER_DISCARD, which is what the KHR-GL43.vertex_attrib_binding
// cases do: the captured record IS the fetched vertex, so "the binding state did
// not reach the draw" and "the draw fetched the wrong bytes" are distinguishable
// from each other and from "the capture did not run" (the buffer is pre-filled
// with a poison value).

#include <cstdio>
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

        constexpr float kPoison = -1234.0f;

        GLuint CompileShader(GLenum type, const std::string& source, std::string* log) {
            const GLuint shader = glCreateShader(type);
            const char* text = source.c_str();
            glShaderSource(shader, 1, &text, nullptr);
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

        // A vertex-only capture program, exactly how the CTS builds one: the varying
        // names are declared before the link and the fragment stage is absent because
        // the draw runs under GL_RASTERIZER_DISCARD.
        GLuint BuildCaptureProgram(const std::string& vertexSource, const std::vector<const char*>& xfbVaryings,
                                   std::string* log) {
            const GLuint vertexShader = CompileShader(GL_VERTEX_SHADER, vertexSource, log);
            if (vertexShader == 0) return 0;
            const GLuint program = glCreateProgram();
            glAttachShader(program, vertexShader);
            if (!xfbVaryings.empty()) {
                glTransformFeedbackVaryings(program, static_cast<GLsizei>(xfbVaryings.size()), xfbVaryings.data(),
                                            GL_INTERLEAVED_ATTRIBS);
            }
            glLinkProgram(program);
            glDeleteShader(vertexShader);
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

        // Four float inputs at locations 0..3, captured as four vec4s per vertex.
        // Locations the test does not feed keep their current-attribute value, which
        // every scenario sets to a known constant first.
        std::string CaptureVertexSource() {
            return R"(#version 430 core
layout(location = 0) in vec4 vs_in_attrib0;
layout(location = 1) in vec4 vs_in_attrib1;
layout(location = 2) in vec4 vs_in_attrib2;
layout(location = 3) in vec4 vs_in_attrib3;
out StageData {
  vec4 attrib0;
  vec4 attrib1;
  vec4 attrib2;
  vec4 attrib3;
} vs_out;
void main() {
  vs_out.attrib0 = vs_in_attrib0;
  vs_out.attrib1 = vs_in_attrib1;
  vs_out.attrib2 = vs_in_attrib2;
  vs_out.attrib3 = vs_in_attrib3;
}
)";
        }

        std::vector<const char*> CaptureVaryingNames() {
            return {"StageData.attrib0", "StageData.attrib1", "StageData.attrib2", "StageData.attrib3"};
        }

        // Runs `vertexCount` x `instanceCount` points through the capture program and
        // returns the interleaved floats (16 per point: four vec4s).
        std::vector<float> CapturePoints(GLuint program, GLuint xfbBuffer, int vertexCount, int instanceCount) {
            const std::size_t floats = static_cast<std::size_t>(vertexCount) * instanceCount * 16;
            std::vector<float> poison(floats, kPoison);
            glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, xfbBuffer);
            glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, static_cast<GLsizeiptr>(floats * sizeof(float)), poison.data(),
                         GL_DYNAMIC_DRAW);

            glEnable(GL_RASTERIZER_DISCARD);
            glUseProgram(program);
            glBeginTransformFeedback(GL_POINTS);
            glDrawArraysInstanced(GL_POINTS, 0, vertexCount, instanceCount);
            glEndTransformFeedback();
            glDisable(GL_RASTERIZER_DISCARD);

            std::vector<float> data(floats, kPoison);
            glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, static_cast<GLsizeiptr>(floats * sizeof(float)),
                               data.data());
            glUseProgram(0);
            return data;
        }

        // point p, attribute a, component c
        float At(const std::vector<float>& data, int point, int attrib, int component) {
            const std::size_t index = static_cast<std::size_t>(point) * 16 + attrib * 4 + component;
            return index < data.size() ? data[index] : kPoison;
        }

        void ResetCurrentAttribs() {
            for (GLuint i = 0; i < 4; ++i) {
                glVertexAttrib4f(i, 0.0f, 0.0f, 0.0f, 0.0f);
            }
        }

        ::testing::AssertionResult Vec4Is(const std::vector<float>& data, int point, int attrib, float x, float y,
                                          float z, float w) {
            const float gx = At(data, point, attrib, 0);
            const float gy = At(data, point, attrib, 1);
            const float gz = At(data, point, attrib, 2);
            const float gw = At(data, point, attrib, 3);
            const float tolerance = 0.01f;
            auto close = [tolerance](float a, float b) { return (a - b) < tolerance && (b - a) < tolerance; };
            if (close(gx, x) && close(gy, y) && close(gz, z) && close(gw, w)) {
                return ::testing::AssertionSuccess();
            }
            return ::testing::AssertionFailure()
                   << "point " << point << " attribute " << attrib << " is (" << gx << ", " << gy << ", " << gz << ", "
                   << gw << "), expected (" << x << ", " << y << ", " << z << ", " << w << ")";
        }

    } // namespace

    class VertexAttribBindingScenario : public ScenarioTest {
    protected:
        void SetUp() override {
            ScenarioTest::SetUp();
            if (!Ready()) return;
            m_program = BuildCaptureProgram(CaptureVertexSource(), CaptureVaryingNames(), &m_log);
            ASSERT_NE(m_program, 0u) << "capture program did not link: " << m_log;
            glGenVertexArrays(1, &m_vao);
            glGenBuffers(1, &m_xfbo);
            glBindVertexArray(m_vao);
        }

        void TearDown() override {
            if (!Ready()) return;
            glBindVertexArray(0);
            glDeleteVertexArrays(1, &m_vao);
            glDeleteBuffers(1, &m_xfbo);
            glDeleteProgram(m_program);
        }

        GLuint m_program = 0;
        GLuint m_vao = 0;
        GLuint m_xfbo = 0;
        std::string m_log;
    };

    // glVertexAttribFormat + glBindVertexBuffer + glVertexAttribBinding, in the order
    // the CTS uses (buffer first, then format, then binding), must feed the draw.
    TEST_F(VertexAttribBindingScenario, FormatAndBindingFeedTheDraw) {
        if (!Ready()) GTEST_SKIP();
        ResetCurrentAttribs();

        const float vertices[] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f};
        GLuint vbo = 0;
        glGenBuffers(1, &vbo);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        glBindVertexBuffer(0, vbo, 0, 12);
        glVertexAttribFormat(1, 3, GL_FLOAT, GL_FALSE, 0);
        glVertexAttribBinding(1, 0);
        glEnableVertexAttribArray(1);

        const std::vector<float> data = CapturePoints(m_program, m_xfbo, 2, 1);
        EXPECT_TRUE(Vec4Is(data, 0, 1, 1.0f, 2.0f, 3.0f, 1.0f));
        EXPECT_TRUE(Vec4Is(data, 1, 1, 4.0f, 5.0f, 6.0f, 1.0f));
        // An attribute nothing configured still reports its current value.
        EXPECT_TRUE(Vec4Is(data, 0, 0, 0.0f, 0.0f, 0.0f, 0.0f));

        glDisableVertexAttribArray(1);
        glDeleteBuffers(1, &vbo);
    }

    // The reverse order - format and binding declared before any buffer exists on the
    // binding point - has to resolve to the same thing once glBindVertexBuffer lands.
    TEST_F(VertexAttribBindingScenario, FormatBeforeBufferStillResolves) {
        if (!Ready()) GTEST_SKIP();
        ResetCurrentAttribs();

        const float vertices[] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f};
        GLuint vbo = 0;
        glGenBuffers(1, &vbo);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        glVertexAttribBinding(2, 3);
        glVertexAttribFormat(2, 2, GL_FLOAT, GL_FALSE, 4);
        glEnableVertexAttribArray(2);
        glBindVertexBuffer(3, vbo, 0, 12);

        const std::vector<float> data = CapturePoints(m_program, m_xfbo, 2, 1);
        EXPECT_TRUE(Vec4Is(data, 0, 2, 2.0f, 3.0f, 0.0f, 1.0f));
        EXPECT_TRUE(Vec4Is(data, 1, 2, 5.0f, 6.0f, 0.0f, 1.0f));

        glDisableVertexAttribArray(2);
        glDeleteBuffers(1, &vbo);
    }

    // GL 4.6 core 10.3.1: a binding point's stride is the byte distance between
    // consecutive elements, and zero means every vertex reads the SAME element. That
    // is the opposite of glVertexAttribPointer's stride 0, which means "tightly
    // packed" - the two spellings must not be collapsed into one another.
    TEST_F(VertexAttribBindingScenario, BindingStrideZeroRepeatsOneElement) {
        if (!Ready()) GTEST_SKIP();
        ResetCurrentAttribs();

        const float vertices[] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f};
        GLuint vbo = 0;
        glGenBuffers(1, &vbo);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        glVertexAttribFormat(0, 4, GL_FLOAT, GL_FALSE, 0);
        glVertexAttribBinding(0, 5);
        glBindVertexBuffer(5, vbo, 16, 0);
        glEnableVertexAttribArray(0);

        const std::vector<float> data = CapturePoints(m_program, m_xfbo, 2, 1);
        EXPECT_TRUE(Vec4Is(data, 0, 0, 5.0f, 6.0f, 7.0f, 8.0f));
        EXPECT_TRUE(Vec4Is(data, 1, 0, 5.0f, 6.0f, 7.0f, 8.0f));

        glDisableVertexAttribArray(0);
        glDeleteBuffers(1, &vbo);
    }

    // The pointer API keeps its own meaning of stride 0 (tightly packed) even though
    // it is defined in terms of the binding model - the negative control for the
    // scenario above.
    TEST_F(VertexAttribBindingScenario, PointerStrideZeroStaysTightlyPacked) {
        if (!Ready()) GTEST_SKIP();
        ResetCurrentAttribs();

        const float vertices[] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f};
        GLuint vbo = 0;
        glGenBuffers(1, &vbo);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

        glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, nullptr);
        glEnableVertexAttribArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        const std::vector<float> data = CapturePoints(m_program, m_xfbo, 2, 1);
        EXPECT_TRUE(Vec4Is(data, 0, 0, 1.0f, 2.0f, 3.0f, 4.0f));
        EXPECT_TRUE(Vec4Is(data, 1, 0, 5.0f, 6.0f, 7.0f, 8.0f));

        glDisableVertexAttribArray(0);
        glDeleteBuffers(1, &vbo);
    }

    // glVertexBindingDivisor is per BINDING POINT: it has to reach every attribute
    // pointed at that binding, and the instance step must honour the divisor rather
    // than advancing once per instance.
    TEST_F(VertexAttribBindingScenario, BindingDivisorAppliesToEveryAttributeOnThePoint) {
        if (!Ready()) GTEST_SKIP();
        ResetCurrentAttribs();

        const float vertices[] = {10.0f, 20.0f, 30.0f, 40.0f};
        GLuint vbo = 0;
        glGenBuffers(1, &vbo);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        glVertexAttribFormat(0, 1, GL_FLOAT, GL_FALSE, 0);
        glVertexAttribFormat(1, 1, GL_FLOAT, GL_FALSE, 4);
        glVertexAttribBinding(0, 4);
        glVertexAttribBinding(1, 4);
        glBindVertexBuffer(4, vbo, 0, 8);
        glVertexBindingDivisor(4, 2);
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);

        // The divisor is per binding point, so it has to be visible on BOTH attributes
        // pointed at it - and this query is what separates "the frontend never resolved
        // it" from "the backend did not apply it".
        GLint divisor = -1;
        glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_DIVISOR, &divisor);
        EXPECT_EQ(divisor, 2);
        divisor = -1;
        glGetVertexAttribiv(1, GL_VERTEX_ATTRIB_ARRAY_DIVISOR, &divisor);
        EXPECT_EQ(divisor, 2);

        // 1 vertex x 4 instances, divisor 2: instances 0,1 read element 0 and
        // instances 2,3 read element 1.
        const std::vector<float> data = CapturePoints(m_program, m_xfbo, 1, 4);
        EXPECT_TRUE(Vec4Is(data, 0, 0, 10.0f, 0.0f, 0.0f, 1.0f));
        EXPECT_TRUE(Vec4Is(data, 1, 0, 10.0f, 0.0f, 0.0f, 1.0f));
        EXPECT_TRUE(Vec4Is(data, 2, 0, 30.0f, 0.0f, 0.0f, 1.0f));
        EXPECT_TRUE(Vec4Is(data, 3, 0, 30.0f, 0.0f, 0.0f, 1.0f));
        EXPECT_TRUE(Vec4Is(data, 0, 1, 20.0f, 0.0f, 0.0f, 1.0f));
        EXPECT_TRUE(Vec4Is(data, 2, 1, 40.0f, 0.0f, 0.0f, 1.0f));

        glDisableVertexAttribArray(0);
        glDisableVertexAttribArray(1);
        glDeleteBuffers(1, &vbo);
    }

    // Two attributes on one binding point at different relative offsets, plus a
    // binding offset: the fetch address is binding offset + relative offset, and the
    // relative offset must not leak into the binding's own offset.
    TEST_F(VertexAttribBindingScenario, RelativeOffsetComposesWithBindingOffset) {
        if (!Ready()) GTEST_SKIP();
        ResetCurrentAttribs();

        const float vertices[] = {0.0f, 0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f};
        GLuint vbo = 0;
        glGenBuffers(1, &vbo);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        glVertexAttribFormat(0, 2, GL_FLOAT, GL_FALSE, 0);
        glVertexAttribFormat(1, 1, GL_FLOAT, GL_FALSE, 8);
        glVertexAttribBinding(0, 1);
        glVertexAttribBinding(1, 1);
        glBindVertexBuffer(1, vbo, 8, 12);
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);

        const std::vector<float> data = CapturePoints(m_program, m_xfbo, 2, 1);
        EXPECT_TRUE(Vec4Is(data, 0, 0, 1.0f, 2.0f, 0.0f, 1.0f));
        EXPECT_TRUE(Vec4Is(data, 0, 1, 3.0f, 0.0f, 0.0f, 1.0f));
        EXPECT_TRUE(Vec4Is(data, 1, 0, 4.0f, 5.0f, 0.0f, 1.0f));
        EXPECT_TRUE(Vec4Is(data, 1, 1, 6.0f, 0.0f, 0.0f, 1.0f));

        glDisableVertexAttribArray(0);
        glDisableVertexAttribArray(1);
        glDeleteBuffers(1, &vbo);
    }

    // The KHR-GL43.vertex_attrib_binding.basic-input* capture program verbatim: a
    // 16-element vec4 input ARRAY at location 0, copied element by element into a
    // 16-element array inside an output interface block, all 16 members captured.
    // Every one of the 17 basic-input* cases is built on it, so a backend that cannot
    // produce this program fails all of them with "the draw captured zeros" and no
    // other symptom.
    TEST_F(VertexAttribBindingScenario, InputArrayCaptureProgramFeedsTheDraw) {
        if (!Ready()) GTEST_SKIP();

        const std::string vs = R"(#version 430 core
layout(location = 0) in vec4 vs_in_attrib[16];
out StageData {
  vec4 attrib[16];
} vs_out;
void main() {
  for (int i = 0; i < vs_in_attrib.length(); ++i) {
    vs_out.attrib[i] = vs_in_attrib[i];
  }
}
)";
        std::vector<std::string> names;
        for (int i = 0; i < 16; ++i) names.push_back("StageData.attrib[" + std::to_string(i) + "]");
        std::vector<const char*> varyings;
        for (const auto& n : names) varyings.push_back(n.c_str());

        std::string log;
        const GLuint program = BuildCaptureProgram(vs, varyings, &log);
        ASSERT_NE(program, 0u) << "capture program did not link: " << log;

        for (GLuint i = 0; i < 16; ++i) glVertexAttrib4f(i, 0.0f, 0.0f, 0.0f, 0.0f);

        const float vertices[] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f};
        GLuint vbo = 0;
        glGenBuffers(1, &vbo);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        glBindVertexBuffer(0, vbo, 0, 12);
        glVertexAttribFormat(1, 3, GL_FLOAT, GL_FALSE, 0);
        glVertexAttribBinding(1, 0);
        glEnableVertexAttribArray(1);

        // 16 vec4s per point rather than the 4 the shared helper assumes.
        constexpr std::size_t kFloatsPerPoint = 64;
        std::vector<float> poison(kFloatsPerPoint * 2, kPoison);
        glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, m_xfbo);
        glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, static_cast<GLsizeiptr>(poison.size() * sizeof(float)),
                     poison.data(), GL_DYNAMIC_DRAW);
        glEnable(GL_RASTERIZER_DISCARD);
        glUseProgram(program);
        glBeginTransformFeedback(GL_POINTS);
        glDrawArrays(GL_POINTS, 0, 2);
        glEndTransformFeedback();
        glDisable(GL_RASTERIZER_DISCARD);

        std::vector<float> data(poison.size(), kPoison);
        glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0,
                           static_cast<GLsizeiptr>(data.size() * sizeof(float)), data.data());
        glUseProgram(0);

        // Element 0 of the array has no enabled array behind it, so it must deliver the
        // current generic attribute value set above - including its w, which is 0 here and
        // NOT the 1 an unwritten vec4 input defaults to.
        EXPECT_FLOAT_EQ(data[0], 0.0f);
        EXPECT_FLOAT_EQ(data[3], 0.0f);

        // attribute 1 of point 0 and of point 1.
        EXPECT_FLOAT_EQ(data[4], 1.0f);
        EXPECT_FLOAT_EQ(data[5], 2.0f);
        EXPECT_FLOAT_EQ(data[6], 3.0f);
        EXPECT_FLOAT_EQ(data[7], 1.0f);
        EXPECT_FLOAT_EQ(data[kFloatsPerPoint + 4], 4.0f);
        EXPECT_FLOAT_EQ(data[kFloatsPerPoint + 5], 5.0f);
        EXPECT_FLOAT_EQ(data[kFloatsPerPoint + 6], 6.0f);
        EXPECT_FLOAT_EQ(data[kFloatsPerPoint + 7], 1.0f);

        glDisableVertexAttribArray(1);
        glDeleteBuffers(1, &vbo);
        glDeleteProgram(program);
    }

    // Same program, but every one of the 16 elements is asked for a DIFFERENT current value.
    //
    // An input array occupies one location per element (GL 4.6 core 11.1.1), so `in vec4 a[16]`
    // at location 0 is active on 0..15 - and the whole location span is what a backend reads to
    // decide which attributes need their current value pushed. Reflection used to record the
    // span of the ELEMENT type only, so a 16-element array claimed exactly one location: every
    // element above the first silently read the (0,0,0,1) an unwritten input defaults to instead
    // of the value glVertexAttrib4f had set. The test above could not see it, because the only
    // element it reads a current value from is element 0 - the one location the array did claim.
    TEST_F(VertexAttribBindingScenario, EveryInputArrayElementGetsItsOwnCurrentValue) {
        if (!Ready()) GTEST_SKIP();

        const std::string vs = R"(#version 430 core
layout(location = 0) in vec4 vs_in_attrib[16];
out StageData {
  vec4 attrib[16];
} vs_out;
void main() {
  for (int i = 0; i < vs_in_attrib.length(); ++i) {
    vs_out.attrib[i] = vs_in_attrib[i];
  }
}
)";
        std::vector<std::string> names;
        for (int i = 0; i < 16; ++i) names.push_back("StageData.attrib[" + std::to_string(i) + "]");
        std::vector<const char*> varyings;
        for (const auto& n : names) varyings.push_back(n.c_str());

        std::string log;
        const GLuint program = BuildCaptureProgram(vs, varyings, &log);
        ASSERT_NE(program, 0u) << "capture program did not link: " << log;

        // Distinct in every component, and never (0,0,0,1): the value an element that was
        // skipped would report has to be distinguishable from every value that was asked for.
        for (GLuint i = 0; i < 16; ++i) {
            const float base = static_cast<float>(i) + 1.0f;
            glVertexAttrib4f(i, base, base + 100.0f, base + 200.0f, base + 300.0f);
        }

        constexpr std::size_t kFloatsPerPoint = 64;
        std::vector<float> poison(kFloatsPerPoint, kPoison);
        glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, m_xfbo);
        glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, static_cast<GLsizeiptr>(poison.size() * sizeof(float)),
                     poison.data(), GL_DYNAMIC_DRAW);
        glEnable(GL_RASTERIZER_DISCARD);
        glUseProgram(program);
        glBeginTransformFeedback(GL_POINTS);
        glDrawArrays(GL_POINTS, 0, 1);
        glEndTransformFeedback();
        glDisable(GL_RASTERIZER_DISCARD);

        std::vector<float> data(poison.size(), kPoison);
        glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0,
                           static_cast<GLsizeiptr>(data.size() * sizeof(float)), data.data());
        glUseProgram(0);

        for (int element = 0; element < 16; ++element) {
            const float base = static_cast<float>(element) + 1.0f;
            EXPECT_FLOAT_EQ(data[element * 4 + 0], base) << "element " << element;
            EXPECT_FLOAT_EQ(data[element * 4 + 1], base + 100.0f) << "element " << element;
            EXPECT_FLOAT_EQ(data[element * 4 + 2], base + 200.0f) << "element " << element;
            EXPECT_FLOAT_EQ(data[element * 4 + 3], base + 300.0f) << "element " << element;
        }

        for (GLuint i = 0; i < 16; ++i) glVertexAttrib4f(i, 0.0f, 0.0f, 0.0f, 0.0f);
        glDeleteProgram(program);
    }

} // namespace MGITest
