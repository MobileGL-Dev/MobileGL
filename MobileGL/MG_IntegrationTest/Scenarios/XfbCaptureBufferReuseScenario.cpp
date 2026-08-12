// MobileGL - MobileGL/MG_IntegrationTest/Scenarios/XfbCaptureBufferReuseScenario.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// ONE capture buffer, SEVERAL capture spans - the shape most KHR-GL4x cases that
// use transform feedback as a readback channel are built on. They allocate the
// capture buffer once in a setup step and then run span after span through it,
// so a defect that only shows from the second span onwards fails the whole case
// while the first span (and every single-span scenario in this suite) stays
// green. The first thing checked here is therefore not the capture itself but
// that the bytes the capture wrote are the bytes the readback reads.
//
// Two ways of reusing the buffer, because they exercise different machinery:
//
//   * respecified between spans (glBufferData while the buffer is still bound to
//     the transform-feedback binding point), which is what a test helper that
//     poisons its capture buffer before every span does;
//   * allocated ONCE with immutable storage and never touched again, which is
//     what KHR-GL45.direct_state_access.vertex_arrays_enable_disable_attributes
//     does - glBufferStorage(4 bytes) in its setup, then two draws.
//
// The negative control (a fresh buffer object per span) is a separate case
// rather than a parameter: it is the configuration that already worked, so it
// has to keep working for the others to mean anything.

#include <cmath>
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
        // One vec4 per point, one point per draw.
        constexpr std::size_t kCaptureFloats = 4;
        constexpr std::size_t kCaptureBytes = kCaptureFloats * sizeof(float);

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

        // Vertex-only capture program: whatever the draw fetched at location 0 comes
        // straight back out through the capture. Runs under GL_RASTERIZER_DISCARD, so
        // there is no fragment stage.
        GLuint BuildCaptureProgram(std::string* log) {
            const std::string vertexSource = R"(#version 430 core
layout(location = 0) in vec4 vs_in_value;
out vec4 vs_out_value;
void main() {
  vs_out_value = vs_in_value;
}
)";
            const GLuint vertexShader = CompileShader(GL_VERTEX_SHADER, vertexSource, log);
            if (vertexShader == 0) return 0;
            const GLuint program = glCreateProgram();
            glAttachShader(program, vertexShader);
            const char* varying = "vs_out_value";
            glTransformFeedbackVaryings(program, 1, &varying, GL_INTERLEAVED_ATTRIBS);
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

        class XfbCaptureBufferReuseScenario : public ScenarioTest {
        protected:
            void SetUp() override {
                ScenarioTest::SetUp();
                if (!Ready()) return;

                std::string log;
                m_program = BuildCaptureProgram(&log);
                ASSERT_NE(m_program, 0u) << "capture program failed to build: " << log;

                glGenVertexArrays(1, &m_vao);
                glBindVertexArray(m_vao);
                glGenBuffers(1, &m_vbo);
                glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
                glBufferData(GL_ARRAY_BUFFER, kCaptureBytes, nullptr, GL_DYNAMIC_DRAW);
                glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, nullptr);
                glEnableVertexAttribArray(0);
                glBindBuffer(GL_ARRAY_BUFFER, 0);
            }

            void TearDown() override {
                if (!Ready()) return;
                glBindVertexArray(0);
                if (m_vbo != 0) glDeleteBuffers(1, &m_vbo);
                if (m_vao != 0) glDeleteVertexArrays(1, &m_vao);
                if (m_program != 0) glDeleteProgram(m_program);
                glUseProgram(0);
                ScenarioTest::TearDown();
            }

            // The vertex the next span will fetch and capture.
            void SetVertex(float value) {
                const float data[kCaptureFloats] = {value, value + 1.0f, value + 2.0f, value + 3.0f};
                glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
                glBufferSubData(GL_ARRAY_BUFFER, 0, kCaptureBytes, data);
                glBindBuffer(GL_ARRAY_BUFFER, 0);
            }

            // One capture span over the buffer currently bound to capture point 0.
            void RunSpan() {
                glEnable(GL_RASTERIZER_DISCARD);
                glUseProgram(m_program);
                glBindVertexArray(m_vao);
                glBeginTransformFeedback(GL_POINTS);
                glDrawArrays(GL_POINTS, 0, 1);
                glEndTransformFeedback();
                glDisable(GL_RASTERIZER_DISCARD);
                glUseProgram(0);
            }

            static ::testing::AssertionResult CapturedIs(const float* data, float value) {
                for (std::size_t i = 0; i < kCaptureFloats; ++i) {
                    const float expected = value + static_cast<float>(i);
                    const float got = data[i];
                    // isfinite first: every ordered comparison against a NaN is false, so a
                    // pair of one-sided range tests REPORTS SUCCESS for uninitialised
                    // storage that happens to read as NaN - which is exactly the failure
                    // these scenarios exist to catch.
                    if (!std::isfinite(got) || std::fabs(got - expected) > 0.01f) {
                        return ::testing::AssertionFailure()
                               << "component " << i << " is " << got << ", expected " << expected
                               << (got == kPoison ? " (the capture never reached these bytes)" : "");
                    }
                }
                return ::testing::AssertionSuccess();
            }

            GLuint m_program = 0;
            GLuint m_vao = 0;
            GLuint m_vbo = 0;
        };

        // The negative control: one buffer object per span. This is the configuration
        // every multi-span scenario in this suite works around the others with, so it
        // has to hold or nothing below is interpretable.
        TEST_F(XfbCaptureBufferReuseScenario, EverySpanIntoABufferObjectOfItsOwn) {
            if (!Ready()) GTEST_SKIP();

            for (int span = 0; span < 3; ++span) {
                const float value = 10.0f * static_cast<float>(span + 1);
                const std::vector<float> poison(kCaptureFloats, kPoison);

                GLuint xfbBuffer = 0;
                glGenBuffers(1, &xfbBuffer);
                glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, xfbBuffer);
                glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, kCaptureBytes, poison.data(), GL_DYNAMIC_DRAW);

                SetVertex(value);
                RunSpan();

                float readback[kCaptureFloats] = {kPoison, kPoison, kPoison, kPoison};
                glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, kCaptureBytes, readback);
                EXPECT_TRUE(CapturedIs(readback, value)) << "span " << span;

                glDeleteBuffers(1, &xfbBuffer);
            }
            EXPECT_EQ(glGetError(), GL_NO_ERROR);
        }

        // The same three spans through ONE buffer object, respecified before each of
        // them WHILE it is bound to capture point 0 - a helper poisoning its capture
        // buffer, which is what makes "captured nothing" legible in the first place.
        //
        // A respecification is free to replace the storage underneath (that is what
        // orphaning is), and on a buffer whose bytes the backend has already handed
        // the frontend a pointer into, the replacement has to reach that pointer too.
        // It did not: the capture wrote the new storage and the readback kept reading
        // the old one, so every span after the first came back poison.
        TEST_F(XfbCaptureBufferReuseScenario, EverySpanIntoOneRespecifiedBufferObject) {
            if (!Ready()) GTEST_SKIP();

            GLuint xfbBuffer = 0;
            glGenBuffers(1, &xfbBuffer);
            glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, xfbBuffer);

            for (int span = 0; span < 3; ++span) {
                const float value = 10.0f * static_cast<float>(span + 1);
                const std::vector<float> poison(kCaptureFloats, kPoison);
                glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, kCaptureBytes, poison.data(), GL_DYNAMIC_DRAW);

                SetVertex(value);
                RunSpan();

                float readback[kCaptureFloats] = {kPoison, kPoison, kPoison, kPoison};
                glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, kCaptureBytes, readback);
                EXPECT_TRUE(CapturedIs(readback, value)) << "span " << span;
            }

            glDeleteBuffers(1, &xfbBuffer);
            EXPECT_EQ(glGetError(), GL_NO_ERROR);
        }

        // A respecification that CHANGES the size, which is the case a re-pointing
        // that only handled same-size storage would still get wrong - and, before the
        // fix, the case that wrote the new (larger) contents through a mapping sized
        // for the old ones.
        TEST_F(XfbCaptureBufferReuseScenario, ARespecificationMayChangeTheCaptureBufferSize) {
            if (!Ready()) GTEST_SKIP();

            GLuint xfbBuffer = 0;
            glGenBuffers(1, &xfbBuffer);
            glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, xfbBuffer);

            // Sized for one point, then for four, then back down to one.
            const std::size_t pointCapacity[] = {1, 4, 1};
            for (int span = 0; span < 3; ++span) {
                const float value = 10.0f * static_cast<float>(span + 1);
                const std::size_t floats = kCaptureFloats * pointCapacity[span];
                const std::vector<float> poison(floats, kPoison);
                glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, static_cast<GLsizeiptr>(floats * sizeof(float)),
                             poison.data(), GL_DYNAMIC_DRAW);

                SetVertex(value);
                RunSpan();

                std::vector<float> readback(floats, kPoison);
                glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0,
                                   static_cast<GLsizeiptr>(floats * sizeof(float)), readback.data());
                EXPECT_TRUE(CapturedIs(readback.data(), value)) << "span " << span;
                // The bytes past the one point the draw produced must still be the
                // poison the respecification put there, not whatever the previous
                // (differently sized) storage held.
                for (std::size_t i = kCaptureFloats; i < floats; ++i) {
                    EXPECT_FLOAT_EQ(readback[i], kPoison) << "span " << span << " float " << i;
                }
            }

            glDeleteBuffers(1, &xfbBuffer);
            EXPECT_EQ(glGetError(), GL_NO_ERROR);
        }

        // The KHR-GL45.direct_state_access.vertex_arrays_enable_disable_attributes
        // shape: the capture buffer gets IMMUTABLE storage once, in a setup step, and
        // is never respecified - two spans simply run through it, each read back with
        // glMapBuffer. Nothing here may depend on a respecification to reset the
        // capture: glBeginTransformFeedback does that on its own.
        TEST_F(XfbCaptureBufferReuseScenario, EverySpanIntoOneImmutableStorageBuffer) {
            if (!Ready()) GTEST_SKIP();

            GLuint xfbBuffer = 0;
            glGenBuffers(1, &xfbBuffer);
            glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, xfbBuffer);
            // Poisoned at creation - the storage is immutable, so this is the only chance to
            // put a recognisable value there, and without it a span that captured nothing
            // would be indistinguishable from one that captured the right thing whenever the
            // untouched bytes happened to read back as the expected number.
            const std::vector<float> poison(kCaptureFloats, kPoison);
            glBufferStorage(GL_TRANSFORM_FEEDBACK_BUFFER, kCaptureBytes, poison.data(), GL_MAP_READ_BIT);
            ASSERT_EQ(glGetError(), GL_NO_ERROR) << "glBufferStorage on the capture buffer";
            glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, xfbBuffer);

            for (int span = 0; span < 3; ++span) {
                const float value = 10.0f * static_cast<float>(span + 1);
                SetVertex(value);
                RunSpan();

                const void* mapped = glMapBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, GL_READ_ONLY);
                ASSERT_NE(mapped, nullptr) << "span " << span << ": glMapBuffer returned null";
                float readback[kCaptureFloats] = {kPoison, kPoison, kPoison, kPoison};
                std::memcpy(readback, mapped, kCaptureBytes);
                glUnmapBuffer(GL_TRANSFORM_FEEDBACK_BUFFER);
                EXPECT_TRUE(CapturedIs(readback, value)) << "span " << span;
            }

            glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, 0);
            glDeleteBuffers(1, &xfbBuffer);
            EXPECT_EQ(glGetError(), GL_NO_ERROR);
        }

    } // namespace
} // namespace MGITest
